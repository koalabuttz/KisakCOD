"""Unit-test the bundled zlib compile step of the fastfile I/O harness.

Only the command line that ``build_zlib`` hands to the compiler is checked. The
zlib sources are empty placeholder files and ``subprocess.run`` is mocked, so the
test needs no assets, no compiler and no full build; the complete fastfile
regression that actually links zlib is ``test_linux_fastfile_io.py``.

The regression this covers: a C++ driver such as ``clang++-19`` used to be
mapped to a C sibling by stripping a trailing ``++``, which does not work for
versioned names, so zlib was compiled as C++ and failed on K&R syntax. The
compile now always asks for the C front end with ``-x c`` and keeps the chosen
driver unchanged.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_linux_fastfile_io as harness  # noqa: E402  (path set above)


class _RecordingRun:
    """Stand-in for ``subprocess.run`` that records argv and never compiles."""

    def __init__(self, returncode: int = 0) -> None:
        self.returncode = returncode
        self.calls = []

    def __call__(self, argv, *args, **kwargs):
        self.calls.append([str(word) for word in argv])
        return subprocess.CompletedProcess(argv, self.returncode)

    def argv_for(self, source: Path):
        for argv in self.calls:
            if str(source) in argv:
                return argv
        raise AssertionError(f"no compile was issued for {source}")


class BuildZlibDriverTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="kisakcod-fastfile-driver-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.library = self.root / "deps" / "zlib"
        self.library.mkdir(parents=True)
        self.out = self.root / "obj"
        self.out.mkdir()
        for name in harness.ZLIB_SOURCES:
            (self.library / f"{name}.c").write_bytes(b"")
        self.sources = [self.library / f"{name}.c" for name in harness.ZLIB_SOURCES]

    def build(self, compiler, override=None, returncode=0):
        run = _RecordingRun(returncode)
        with mock.patch.object(harness.subprocess, "run", run):
            objects = harness.build_zlib(compiler, self.root, self.out, override)
        return run, objects

    def assert_c_mode(self, argv, source: Path) -> None:
        self.assertIn("-x", argv)
        self.assertEqual(argv[argv.index("-x") + 1], "c", "the C front end must be selected")
        self.assertLess(argv.index("-x"), argv.index(str(source)),
                        "-x c must precede the source it applies to")
        self.assertIn("-std=gnu89", argv)
        self.assertLess(argv.index("-x"), argv.index("-std=gnu89"))

    # -- the C front end is always selected --------------------------------

    def test_every_translation_unit_is_compiled_as_c(self) -> None:
        run, objects = self.build(["clang++"])
        self.assertEqual(len(run.calls), len(self.sources))
        self.assertEqual(objects, [str(self.out / f"{name}.o") for name in harness.ZLIB_SOURCES])
        for source in self.sources:
            argv = run.argv_for(source)
            self.assert_c_mode(argv, source)
            self.assertEqual(argv[0], "clang++")
            self.assertIn("-m32", argv)
            self.assertIn("-c", argv)
            self.assertEqual(argv[-2:], ["-o", str(self.out / (source.stem + ".o"))])

    def test_plain_c_driver_is_accepted(self) -> None:
        run, _ = self.build(["gcc"])
        self.assert_c_mode(run.argv_for(self.sources[0]), self.sources[0])

    def test_versioned_cxx_driver_is_used_unchanged(self) -> None:
        # The regression: clang++-19 has no clang-19 sibling lookup, so stripping
        # a trailing "++" never applied. The driver must be used as given.
        run, _ = self.build(["/usr/bin/clang++-19", "--target=x86_64-linux-gnu"])
        self.assertEqual(len(run.calls), len(self.sources))
        for source in self.sources:
            argv = run.argv_for(source)
            self.assertEqual(argv[0], "/usr/bin/clang++-19")
            self.assert_c_mode(argv, source)
            self.assertIn("--target=x86_64-linux-gnu", argv)
            for word in argv:
                # No sibling rewrite, and no bare "--++"-derived fallback name.
                self.assertNotIn("clang-19", word)
                self.assertNotEqual(word, "clang")

    def test_versioned_c_driver_is_used_unchanged(self) -> None:
        run, _ = self.build(["gcc-13"])
        self.assertEqual(run.argv_for(self.sources[0])[0], "gcc-13")

    # -- the CC override ---------------------------------------------------

    def test_cc_override_replaces_the_harness_driver(self) -> None:
        run, _ = self.build(["clang++-19"], override="ccache /opt/bin/gcc-12")
        for source in self.sources:
            argv = run.argv_for(source)
            self.assertEqual(argv[:2], ["ccache", "/opt/bin/gcc-12"])
            self.assert_c_mode(argv, source)

    def test_cc_override_reads_the_environment(self) -> None:
        run = _RecordingRun(0)
        with mock.patch.dict(os.environ, {"CC": "clang-18"}, clear=False):
            with mock.patch.object(harness.subprocess, "run", run):
                harness.build_zlib(["clang++-19"], self.root, self.out)
        self.assertEqual(run.argv_for(self.sources[0])[0], "clang-18")

    def test_empty_cc_override_is_rejected(self) -> None:
        run = _RecordingRun(0)
        for override in ("", "   ", "\t"):
            with self.assertRaises(SystemExit) as raised:
                with mock.patch.object(harness.subprocess, "run", run):
                    harness.build_zlib(["clang++"], self.root, self.out, override)
            self.assertIn("CC", str(raised.exception))
        self.assertEqual(run.calls, [], "an empty CC must not compile anything")

    def test_unset_cc_override_keeps_the_harness_driver(self) -> None:
        run = _RecordingRun(0)
        with mock.patch.dict(os.environ, {"CC": ""}, clear=False):
            with mock.patch.object(harness.subprocess, "run", run):
                harness.build_zlib(["clang++-19"], self.root, self.out)
        self.assertEqual(run.calls[0][0], "clang++-19")

    def test_empty_driver_list_fails_closed(self) -> None:
        run = _RecordingRun(0)
        with self.assertRaises(SystemExit):
            with mock.patch.object(harness.subprocess, "run", run):
                harness.build_zlib([], self.root, self.out, "  ")
        self.assertEqual(run.calls, [])

    # -- missing sources and compile failures ------------------------------

    def test_missing_source_is_reported_before_any_compile(self) -> None:
        (self.library / "inflate.c").unlink()
        run = _RecordingRun(0)
        with self.assertRaises(SystemExit) as raised:
            with mock.patch.object(harness.subprocess, "run", run):
                harness.build_zlib(["clang++"], self.root, self.out)
        self.assertIn("inflate.c", str(raised.exception))
        self.assertEqual(run.calls, [], "a missing source must be reported, not compiled")

    def test_compile_failure_stops_the_build(self) -> None:
        run = _RecordingRun(1)
        with mock.patch.object(harness.subprocess, "run", run):
            with self.assertRaises(SystemExit) as raised:
                harness.build_zlib(["clang++-19"], self.root, self.out)
        self.assertIn("failed to compile", str(raised.exception))
        self.assertIn(self.sources[0].name, str(raised.exception))
        self.assertEqual(len(run.calls), 1, "the first failure must stop the build")
        self.assert_c_mode(run.calls[0], self.sources[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
