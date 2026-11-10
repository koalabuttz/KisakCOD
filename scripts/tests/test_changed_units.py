#!/usr/bin/env python3
"""Check which translation units a change is said to reach.

The two whole-tree source scans of the x86_64 client job
(``check_32bit_size_literals.py``, ``check_32bit_layout_offsets.py``) scan, on
a pull request, only the units the change can reach
(``scripts/tools/changed_units.py``). A unit left out by mistake would hide a
finding, so this pins the rule on a made-up source tree and dependency log:
what is selected, and every case in which everything is scanned instead.

Usage:
    python3 scripts/tests/test_changed_units.py
"""

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "tools"))
import changed_units  # noqa: E402


class ReachTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="kisak-changed-units-")
        self.root = Path(self.work.name).resolve()
        files = {
            "src/a/one.cpp": '#include "shared.h"\n#include "one.h"\n',
            "src/a/two.cpp": '#include "shared.h"\n#ifdef __i386__\n#  include <a/only32.h>\n#endif\n',
            "src/a/unbuilt.cpp": '#include "shared.h"\n',
            "src/a/shared.h": "",
            "src/a/one.h": "",
            "src/a/only32.h": "",
            "src/vita/vita_only.h": "",
            "src/vita/vita_only.cpp": '#include "vita_only.h"\n',
        }
        for name, text in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        self.units = {str(self.root / name) for name in ("src/a/one.cpp", "src/a/two.cpp", "src/a/unbuilt.cpp")}
        log = ("obj/one.o: #deps 3, deps mtime 1 (VALID)\n"
               "    ../src/a/one.cpp\n    ../src/a/shared.h\n    ../src/a/one.h\n"
               "\n"
               "obj/two.o: #deps 3, deps mtime 1 (VALID)\n"
               "    ../src/a/two.cpp\n    ../src/a/shared.h\n    /usr/include/stdio.h\n"
               "\n"
               "obj/stale.o: #deps 1, deps mtime 1 (STALE)\n"
               "    ../src/a/unbuilt.cpp\n")
        self.deps = changed_units.parse_deps(log, self.root / "build")

    def tearDown(self):
        self.work.cleanup()

    def reach(self, *changed):
        selected, why = changed_units.reachable(self.units, list(changed), self.deps, self.root)
        if selected is None:
            return None, why
        return sorted(Path(path).name for path in selected), why

    def test_the_dependency_log_is_read(self):
        self.assertEqual(sorted(self.deps), ["obj/one.o", "obj/two.o"])
        self.assertIn(str(self.root / "src/a/one.h"), self.deps["obj/one.o"])
        self.assertIsNone(changed_units.parse_deps("", self.root / "build"))

    def test_a_changed_source_is_scanned_alone(self):
        self.assertEqual(self.reach("src/a/one.cpp")[0], ["one.cpp"])

    def test_a_changed_header_reaches_its_includers_and_every_unbuilt_unit(self):
        self.assertEqual(self.reach("src/a/one.h")[0], ["one.cpp", "unbuilt.cpp"])
        self.assertEqual(self.reach("src/a/shared.h")[0], ["one.cpp", "two.cpp", "unbuilt.cpp"])

    def test_a_changed_unbuilt_unit_is_scanned(self):
        self.assertEqual(self.reach("src/a/unbuilt.cpp")[0], ["unbuilt.cpp"])

    def test_documentation_tests_and_other_platforms_reach_nothing(self):
        selected, why = self.reach("docs/runs.md", "README.md", "tests/runs/fake_game.cpp",
                                   "scripts/tests/test_perf_run.py", "scripts/runs/perf_run.py",
                                   "scripts/platform/vita/CMakeLists.txt", ".github/workflows/build-kisakcod-macos.yaml")
        self.assertEqual(selected, [], why)

    def test_a_file_no_unit_includes_reaches_only_unbuilt_units(self):
        # Another platform's own sources: in no dependency list, named by no
        # source of this build.
        self.assertEqual(self.reach("src/vita/vita_only.cpp")[0], [])
        self.assertEqual(self.reach("src/vita/vita_only.h")[0], ["unbuilt.cpp"])

    def test_everything_is_scanned_when_the_reach_is_not_known(self):
        cases = {
            "a header included under a condition this build does not meet": ("src/a/only32.h",),
            "a CMake file": ("CMakeLists.txt",),
            "a platform's CMake": ("scripts/platform/linux/CMakeLists.txt", "src/a/one.cpp"),
            "the scan itself": ("scripts/tools/check_32bit_size_literals.py",),
            "an allow list": ("scripts/tools/check_32bit_layout_offsets_allow.txt",),
            "this rule": ("scripts/tools/changed_units.py",),
            "the workflow that configures the build": (".github/workflows/build-kisakcod-linux.yaml",),
            "a dependency": ("deps/some_library/source.c",),
        }
        for what, changed in cases.items():
            with self.subTest(what):
                selected, why = self.reach(*changed)
                self.assertIsNone(selected, why)
                self.assertTrue(why)
        selected, why = changed_units.reachable(self.units, None, self.deps, self.root)
        self.assertIsNone(selected)
        selected, why = changed_units.reachable(self.units, ["src/a/one.cpp"], None, self.root)
        self.assertIsNone(selected)

    def test_git_that_cannot_answer_means_everything(self):
        self.assertIsNone(changed_units.changed_files(self.root, "HEAD^1"))  # not a repository
        self.assertIsNone(changed_units.ninja_deps(self.root / "no-build"))


class WorkflowTests(unittest.TestCase):
    def test_only_pull_requests_narrow_the_scans(self):
        text = (ROOT / ".github/workflows/build-kisakcod-linux.yaml").read_text()
        job = text[text.index("\n  x64_client:\n"):text.index("\n  arm64_client:\n")]
        self.assertIn("fetch-depth: 2", job)
        self.assertEqual(job.count('if [ "${{ github.event_name }}" = pull_request ]; then '
                                   'since=(--changed-since HEAD^1); fi'), 2)
        for tool in ("check_32bit_size_literals", "check_32bit_layout_offsets"):
            self.assertIn(f'scripts/tools/{tool}.py --build-dir "$BUILD_DIR" "${{since[@]}}"', job)
        self.assertEqual(text.count("--changed-since"), 2)


if __name__ == "__main__":
    unittest.main()
