#!/usr/bin/env python3
"""Regression guards for the native Linux check runner and its runtime script.

These tests cover the ways ``run_linux_checks.py`` and ``test_linux_runtime.py``
can report success without having proved anything:

  * a planned command that drops the build directory, the compiler handed in on
    the command line, or the compiler's ``-m32`` flag, so a check compiles
    against the wrong target or silently picks up an ambient ``CXX``;
  * a default check set that would need licensed assets or a listening server;
  * a missing build manifest or a build manifest with no native entry being
    treated as a green run;
  * the recorded compile flags losing an include root, or keeping CMake's
    dependency-file outputs, so the runtime self-test builds against something
    other than the project;
  * a failure in the first command of a compile/test pair still running the
    second command, or a failed check being left out of the failure count;
  * a timed-out command leaving its compiler children alive.

Nothing here configures, builds, starts a server, or touches game data. The only
subprocess is one interpreter start that is expected to fail its argument check.
"""

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parents[1]

# Import the siblings by path: the runner and the runtime script live next to
# this file and are not an installed package.
sys.path.insert(0, str(TESTS))

import run_linux_checks as runner  # noqa: E402
import test_linux_runtime as runtime  # noqa: E402


# A compiler that cannot be confused with any default, so a test that finds it
# in an argv proves the caller's value was used rather than a fallback.
CXX = "/opt/toolchain-sentinel/clang++-m32"
AMBIENT_CXX = "/usr/bin/ambient-cxx-must-not-be-used"
BUILD_DIR = Path("/nonexistent-build-linux/debug")
BINARY = Path("/nonexistent-build-linux/debug/bin/KisakCOD-dedi")
PYTHON = "/sentinel/python3"


def default_options(only=(), skip=()):
    return argparse.Namespace(only=set(only), skip=set(skip))


def plan_checks(workdir, cxx=CXX, build_dir=BUILD_DIR, only=(), skip=()):
    return runner.plan_checks(
        default_options(only, skip), Path(workdir), Path(build_dir), BINARY, PYTHON, cxx
    )


def check_named(checks, name):
    for check in checks:
        if check.name == name:
            return check
    raise AssertionError(f"no planned check named {name!r}; planned: {[c.name for c in checks]}")


class FakeProcess:
    """Stand-in for a ``subprocess.Popen`` result used by the runner."""

    def __init__(self, returncode=0, outputs=(("done\n", None),), timeout_first=True):
        self.pid = 4321
        self.returncode = returncode
        self._outputs = [tuple(entry) for entry in outputs]
        self._timeout_first = timeout_first
        self.communicate_calls = 0
        self.killed = False

    def communicate(self, timeout=None):
        self.communicate_calls += 1
        if self._timeout_first and self.communicate_calls == 1 and timeout is not None:
            raise subprocess.TimeoutExpired(cmd="fake", timeout=timeout)
        if self._outputs:
            return self._outputs.pop(0)
        return ("", None)

    def kill(self):
        self.killed = True


class PlannedCommandTests(unittest.TestCase):
    """The plan must carry the caller's build directory and compiler everywhere."""

    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="kisakcod-plan-test-")
        self.addCleanup(self._temporary.cleanup)
        self.workdir = Path(self._temporary.name)
        self.checks = plan_checks(self.workdir)

    def test_runtime_check_always_passes_the_build_directory(self):
        for command in check_named(self.checks, "runtime").commands:
            self.assertIn("--build-dir", command.argv)
            index = command.argv.index("--build-dir")
            self.assertEqual(command.argv[index + 1], str(BUILD_DIR))
        self.assertIn("--build-dir", self.checks[1].commands[0].argv)

    def test_runtime_check_receives_the_explicit_compiler(self):
        argv = check_named(self.checks, "runtime").commands[0].argv
        self.assertIn("--compiler", argv)
        self.assertEqual(argv[argv.index("--compiler") + 1], CXX)

    def test_compiled_checks_receive_the_explicit_compiler_on_the_command_line(self):
        for name, flag in (("debugger", "--cxx"), ("fastfile", "--cxx"), ("net_deadline", "--cxx"),
                           ("huffman", "--cxx"), ("handshake_auth", "--cxx"),
                           ("random_range", "--cxx"), ("gameplay_lifecycle", "--cxx"),
                           ("server_test_clients", "--cxx")):
            argv = check_named(self.checks, name).commands[0].argv
            self.assertIn(flag, argv, f"{name} does not pass {flag}")
            self.assertEqual(argv[argv.index(flag) + 1], CXX, f"{name} uses a compiler other than --cxx")
            self.assertNotIn("clang++", argv, f"{name} falls back to a default compiler")

    def test_handshake_auth_requires_32_bit_execution(self):
        self.assertIn("--m32", check_named(self.checks, "handshake_auth").commands[0].argv)

    def test_server_test_clients_require_32_bit_sanitized_execution(self):
        argv = check_named(self.checks, "server_test_clients").commands[0].argv
        self.assertIn("--m32", argv)
        self.assertIn("--sanitize", argv)

    def test_source_self_tests_compile_with_the_explicit_compiler(self):
        for name in ("sys_fileio", "sys_virtualmem"):
            self.assertEqual(check_named(self.checks, name).commands[0].argv[0], CXX)

    def test_udp_and_lexer_checks_set_their_own_cxx(self):
        udp = check_named(self.checks, "udp").commands[0]
        self.assertEqual(udp.env.get("CXX"), CXX)
        # TMPDIR is redirected into the run's temporary directory so concurrent
        # runs cannot collide on a fixed binary name in /tmp.
        self.assertEqual(Path(udp.env["TMPDIR"]).parent, self.workdir)
        self.assertEqual(check_named(self.checks, "lexer_eof").commands[0].env.get("CXX"), CXX)

    def test_command_environment_wins_over_an_ambient_cxx(self):
        process = FakeProcess(timeout_first=False)
        captured = {}

        def fake_popen(argv, **kwargs):
            captured.update(kwargs)
            captured["argv"] = argv
            return process

        with mock.patch.dict(os.environ, {"CXX": AMBIENT_CXX}):
            command = runner.Command(["bash", "run_tests.sh"], env={"CXX": CXX})
            with mock.patch.object(runner.subprocess, "Popen", side_effect=fake_popen):
                with contextlib.redirect_stdout(io.StringIO()):
                    status, _, _ = runner.run_command(command)

        self.assertEqual(status, "ok")
        self.assertEqual(captured["env"]["CXX"], CXX)
        self.assertNotIn(AMBIENT_CXX, captured["env"].values())

    def test_every_compiler_self_test_includes_m32(self):
        compiled = [command for check in self.checks for command in check.commands
                    if command.argv and command.argv[0] == CXX]
        self.assertEqual(len(compiled), 2, "expected the file I/O and virtual-memory self-tests")
        for command in compiled:
            self.assertIn("-m32", command.argv, f"self-test without -m32: {command.describe()}")

    def test_default_plan_needs_no_assets_and_no_running_server(self):
        names = {check.name for check in self.checks}
        self.assertNotIn("asset_startup", names)
        self.assertNotIn("server_lifecycle", names)
        self.assertEqual({name for name in names if "lifecycle" in name},
                         {"lifecycle_driver", "gameplay_lifecycle"})
        for check in self.checks:
            argv = " ".join(command.describe() for command in check.commands)
            self.assertNotIn("test_linux_asset_startup.py", argv)
            self.assertNotIn("test_linux_server_lifecycle.py", argv)
            # Only bootstrap may depend on an artefact the caller built, and it
            # must be the executable named on the command line.
            if check.name == "bootstrap":
                self.assertEqual(Path(check.requires), BINARY)
            else:
                self.assertIsNone(check.requires, f"{check.name} requires an artefact")

    def test_default_plan_is_the_fifteen_known_checks_in_order(self):
        # main() validates --only/--skip against this list; a plan that drifts
        # from it would let a check be named in help text but never run.
        known = [
            "bootstrap",
            "runtime",
            "lifecycle_driver",
            "handshake_auth",
            "debugger",
            "fastfile",
            "net_deadline",
            "huffman",
            "udp",
            "sys_fileio",
            "sys_virtualmem",
            "lexer_eof",
            "random_range",
            "gameplay_lifecycle",
            "server_test_clients",
            "weapon_def_ownership",
            "bot_driver",
            "database_publication",
        ]
        self.assertEqual([check.name for check in self.checks], known)

    def test_only_and_skip_filter_the_plan(self):
        only_runtime = plan_checks(self.workdir, only=[("runtime")])
        self.assertEqual([check.name for check in only_runtime], ["runtime"])

        without_runtime = plan_checks(self.workdir, skip=["runtime"])
        self.assertNotIn("runtime", [check.name for check in without_runtime])


class RecordedFlagTests(unittest.TestCase):
    """``project_compile_flags`` must copy the build's flags faithfully."""

    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="kisakcod-flags-test-")
        self.addCleanup(self._temporary.cleanup)
        self.build_dir = Path(self._temporary.name)

    def write_commands(self, entries):
        (self.build_dir / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    def test_manifest_without_a_native_entry_raises_instead_of_falling_back(self):
        self.write_commands([{"file": str(ROOT / "src" / "win32" / "win_main.cpp"),
                              "command": "clang++ -c win_main.cpp"}])
        with mock.patch.object(runtime, "BUILD_DIR", self.build_dir):
            with self.assertRaises(ValueError) as raised:
                runtime.project_compile_flags()
        self.assertIn("sys_posix.cpp", str(raised.exception))

    def test_empty_manifest_raises_instead_of_falling_back(self):
        self.write_commands([])
        with mock.patch.object(runtime, "BUILD_DIR", self.build_dir):
            with self.assertRaises(ValueError):
                runtime.project_compile_flags()

    def test_recorded_flags_keep_includes_and_drop_dependency_outputs(self):
        source = str(ROOT / "src" / "linux" / "sys_posix.cpp")
        self.write_commands(
            [
                {
                    "directory": str(self.build_dir),
                    "file": source,
                    "command": (
                        "clang++ -std=gnu++20 -m32 -I/repo/src -I/repo/deps -D_DEBUG "
                        "-isystem /usr/include/sentinel -o CMakeFiles/x.o -c "
                        f"{source} -MD -MT CMakeFiles/x.o.d -MF CMakeFiles/x.o.d -MQ dep"
                    ),
                }
            ]
        )
        with mock.patch.object(runtime, "BUILD_DIR", self.build_dir):
            flags, origin = runtime.project_compile_flags()

        self.assertEqual(origin, str(self.build_dir / "compile_commands.json"))
        self.assertIn("-I/repo/src", flags)
        self.assertIn("-I/repo/deps", flags)
        self.assertEqual(flags[flags.index("-isystem") + 1], "/usr/include/sentinel")
        self.assertIn("-m32", flags)
        for dropped in ("-o", "-MT", "-MF", "-MQ", "-c", "-MD", "-MMD"):
            self.assertNotIn(dropped, flags, f"{dropped} survived the dependency filter")
        joined = " ".join(flags)
        self.assertNotIn("CMakeFiles", joined)
        self.assertNotIn(source, joined)
        self.assertNotIn("clang++", flags)


class FailClosedTests(unittest.TestCase):
    """A failing or missing piece must never be summarised as success."""

    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="kisakcod-run-test-")
        self.addCleanup(self._temporary.cleanup)
        self.workdir = Path(self._temporary.name)

    def test_failed_first_command_stops_a_compile_test_pair(self):
        invoked = []

        def fake_run_command(command):
            invoked.append(command.argv)
            return ("failed", "compiler exploded", 0.1)

        check = runner.Check(
            "sys_fileio",
            "compile then run",
            [
                runner.Command([CXX, "-m32", "sys_fileio.cpp", "-o", "binary"]),
                runner.Command(["binary"]),
            ],
        )
        with mock.patch.object(runner, "run_command", side_effect=fake_run_command):
            with contextlib.redirect_stdout(io.StringIO()):
                results, failures = runner.run_checks([check], None)

        self.assertEqual(len(invoked), 1, "the test binary ran after its compile failed")
        self.assertEqual(failures, 1)
        self.assertEqual(results[0][1], "failed")
        self.assertIn("command 1", results[0][2])

    def test_failures_aggregate_across_checks(self):
        passing = runner.Check("lifecycle_driver", "ok", [runner.Command(["true"])])
        failing = runner.Check("udp", "bad", [runner.Command(["false"])])

        def fake_run_command(command):
            return ("ok", "", 0.0) if command.argv == ["true"] else ("failed", "", 0.0)

        with mock.patch.object(runner, "run_command", side_effect=fake_run_command):
            with contextlib.redirect_stdout(io.StringIO()):
                results, failures = runner.run_checks([passing, failing], None)

        self.assertEqual(failures, 1)
        self.assertEqual([status for _, status, _, _ in results], ["ok", "failed"])

    def test_missing_artefact_is_reported_blocked_not_passed(self):
        check = runner.Check(
            "bootstrap",
            "needs the compiled server",
            [runner.Command(["true"])],
            requires=self.workdir / "bin" / "absent-dedi",
        )
        with mock.patch.object(runner, "run_command", side_effect=AssertionError("must not run")):
            with contextlib.redirect_stdout(io.StringIO()):
                results, failures = runner.run_checks([check], None)

        self.assertEqual(failures, 1)
        self.assertEqual(results[0][1], "blocked")
        self.assertIn("absent-dedi", results[0][2])

    def test_summary_exit_code_follows_the_failure_count(self):
        passed = runner.Check("runtime", "fine", [])
        failed = runner.Check("udp", "broken", [])
        results = [(passed, "ok", "", 1.0), (failed, "failed", "exit 1", 2.0)]

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runner.print_summary(results[:1], 0), 0)
            self.assertEqual(runner.print_summary(results, 1), 1)


class CommandExecutionTests(unittest.TestCase):
    """Timeouts and status mapping for one planned command."""

    def test_timeout_kills_the_whole_process_group(self):
        process = FakeProcess(returncode=-9, outputs=[("partial notes\n", None)], timeout_first=True)
        with mock.patch.object(runner.subprocess, "Popen", return_value=process) as popen:
            with mock.patch.object(runner.os, "getpgid", return_value=98765) as getpgid:
                with mock.patch.object(runner.os, "killpg") as killpg:
                    with contextlib.redirect_stdout(io.StringIO()):
                        status, output, _ = runner.run_command(
                            runner.Command(["slow-check"], timeout=5)
                        )

        self.assertEqual(status, "timeout")
        self.assertEqual(output, "partial notes\n")
        getpgid.assert_called_once_with(process.pid)
        killpg.assert_called_once_with(98765, runner.signal.SIGKILL)
        self.assertEqual(process.communicate_calls, 2)
        # The session is created up front so the kill can reach grandchildren.
        self.assertTrue(popen.call_args.kwargs["start_new_session"])

    def test_nonzero_exit_is_reported_failed(self):
        process = FakeProcess(returncode=3, outputs=[("boom\n", None)], timeout_first=False)
        with mock.patch.object(runner.subprocess, "Popen", return_value=process):
            with contextlib.redirect_stdout(io.StringIO()):
                status, output, _ = runner.run_command(runner.Command(["check"]))

        self.assertEqual(status, "failed")
        self.assertEqual(output, "boom\n")

    def test_unlaunchable_command_is_an_error_not_a_pass(self):
        with mock.patch.object(runner.subprocess, "Popen", side_effect=FileNotFoundError("no such file")):
            with contextlib.redirect_stdout(io.StringIO()):
                status, _, _ = runner.run_command(runner.Command(["/missing/compiler"]))

        self.assertEqual(status, "error")


class MainGuardTests(unittest.TestCase):
    """``main`` must refuse to run the runtime check without a usable manifest."""

    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="kisakcod-main-test-")
        self.addCleanup(self._temporary.cleanup)
        self.build_dir = Path(self._temporary.name)
        self.binary = self.build_dir / "bin" / "KisakCOD-dedi"
        self.binary.parent.mkdir(parents=True, exist_ok=True)
        self.binary.write_bytes(b"")

    def run_main(self, extra_argv):
        argv = ["run_linux_checks.py", *extra_argv]
        captured = io.StringIO()
        with mock.patch.object(sys, "argv", argv):
            with mock.patch.object(runner, "run_checks", side_effect=AssertionError("must not run")):
                with contextlib.redirect_stdout(captured):
                    status = runner.main()
        return status, captured.getvalue()

    def test_runtime_check_without_compile_commands_fails_before_running_anything(self):
        status, output = self.run_main(
            ["--only", "runtime", "--build-dir", str(self.build_dir), "--binary", str(self.binary)]
        )

        self.assertEqual(status, 1)
        self.assertIn("FAIL: missing", output)
        self.assertIn("compile_commands.json", output)
        self.assertIn("CMAKE_EXPORT_COMPILE_COMMANDS", output)
        self.assertNotIn("Summary", output)

    def test_runtime_script_reports_the_missing_native_entry(self):
        (self.build_dir / "compile_commands.json").write_text("[]", encoding="utf-8")

        completed = subprocess.run(
            [sys.executable, str(TESTS / "test_linux_runtime.py"), "--build-dir", str(self.build_dir)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=60,
            check=False,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("sys_posix.cpp", completed.stderr)
        self.assertIn("error:", completed.stderr)
        # It refused to run rather than compiling against the fallback flags.
        self.assertNotIn("built runtime test", completed.stdout)
        self.assertNotIn("compiles cleanly", completed.stdout)


if __name__ == "__main__":
    unittest.main()
