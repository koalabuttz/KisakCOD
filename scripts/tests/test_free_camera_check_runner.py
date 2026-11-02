#!/usr/bin/env python3
"""Contract tests for run_free_camera_checks.py.

These check the runner, not the camera: what it compiles and with which flags,
that the original engine unit is compiled unmodified and without this change's
strict warning set, and that a failed check, a reported failure with a zero exit,
a compile error, a stuck test and a missing source each end in a non-zero exit.
The negative cases run against a temporary copy of exactly the files the checks
include (taken from the compiler's own dependency output), so the real tree is
never edited.
"""

from importlib import util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "tests" / "run_free_camera_checks.py"
CASE_NAMES = ["free_camera", "frame_owner"]

REPORTS_FAILURE_AND_EXITS_ZERO = """\
#include <cstdio>
int main()
{
    std::printf("FAIL forced by the runner contract test\\n");
    return 0;
}
"""

STUCK = """\
#include <unistd.h>
int main()
{
    sleep(120);
    return 0;
}
"""


def load_runner():
    spec = util.spec_from_file_location("run_free_camera_checks", RUNNER)
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_runner(arguments, timeout=600):
    return subprocess.run(
        [sys.executable, str(RUNNER), *[str(argument) for argument in arguments]],
        cwd=str(ROOT),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
    )


class Options:
    def __init__(self, config="Debug", timeout=300, compile_timeout=600, cxx_flag=()):
        self.config = config
        self.timeout = timeout
        self.compile_timeout = compile_timeout
        self.cxx_flag = list(cxx_flag)


class RunnerPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner()

    def plan(self, selected=None, **options):
        workdir = Path("/nonexistent-work")
        return self.runner.plan_checks(Options(**options), workdir, ROOT, "clang++",
                                       set(selected or self.runner.KNOWN_NAMES))

    def test_cases_exist_and_are_named(self):
        self.assertEqual([case.name for case in self.runner.CASES], CASE_NAMES)
        for case in self.runner.CASES:
            for source in case.sources:
                self.assertTrue((ROOT / source).is_file(), source)
        self.assertTrue((ROOT / self.runner.PRODUCTION_UNIT).is_file())

    def test_the_production_unit_is_compiled_first_with_production_flags_only(self):
        checks = self.plan()
        self.assertEqual([check.name for check in checks], [self.runner.OBJECT_CHECK, *CASE_NAMES])
        object_line = checks[0].commands[0].argv
        self.assertIn("-c", object_line)
        self.assertIn(str(ROOT / "src/universal/com_math_anglevectors.cpp"), object_line)
        for flag in ("-m32", "-std=c++20", "-fms-extensions", "-msse2", "-mfpmath=sse", "-DKISAK_MP",
                     "-ffunction-sections"):
            self.assertIn(flag, object_line)
        # The original source is compiled as it is: this change's -Werror must not
        # turn its existing warnings into a failure that invites editing it.
        for flag in self.runner.STRICT_FLAGS:
            self.assertNotIn(flag, object_line)

    def test_the_new_code_is_compiled_strictly_and_linked_with_gc_sections(self):
        for check in self.plan()[1:]:
            line = check.commands[0].argv
            for flag in ("-m32", "-fms-extensions", "-Wall", "-Wextra", "-Werror", "-Wl,--gc-sections"):
                self.assertIn(flag, line, check.name)
            self.assertIn(str(Path("/nonexistent-work") / self.runner.PRODUCTION_OBJECT), line)
            self.assertEqual(check.commands[1].argv, [str(Path("/nonexistent-work") / check.name)])
            self.assertIsNotNone(check.binary)
            # Nothing graphical or engine-wide reaches the line.
            joined = " ".join(line)
            for forbidden in ("SDL", "dxvk", "d3d9", "gfx_d3d", "-lvulkan"):
                self.assertNotIn(forbidden, joined)

    def test_configurations_differ_only_in_optimization(self):
        debug = self.plan(config="Debug")[1].commands[0].argv
        release = self.plan(config="RelWithDebInfo")[1].commands[0].argv
        self.assertEqual([part for part in debug if part != "-O0"], [part for part in release if part != "-O2"])

    def test_selecting_one_case_still_builds_the_object(self):
        checks = self.plan(selected={"frame_owner"})
        self.assertEqual([check.name for check in checks], [self.runner.OBJECT_CHECK, "frame_owner"])

    def test_extra_flags_are_only_what_was_asked_for(self):
        line = self.plan(cxx_flag=["-DPROBE"])[1].commands[0].argv
        self.assertEqual(line.count("-DPROBE"), 1)

    def test_unknown_names_and_bad_timeouts_are_refused(self):
        result = run_runner(["--only", "nope", "--list"])
        self.assertEqual(result.returncode, 2)
        self.assertIn("free_camera", result.stdout)
        self.assertEqual(run_runner(["--timeout", "0", "--list"]).returncode, 2)

    def test_list_runs_nothing(self):
        result = run_runner(["--config", "RelWithDebInfo", "--list"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("ran nothing", result.stdout)
        self.assertIn("-O2", result.stdout)
        self.assertNotIn("Summary", result.stdout)


class RunnerBehaviourTests(unittest.TestCase):
    """Each test drives the real script against a temporary copy of the sources."""

    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner()
        cls.cxx = "clang++"
        if not shutil.which(cls.cxx):
            raise unittest.SkipTest("clang++ is not installed")
        # Every file the checks read, as the compiler itself reports it.
        sources = {cls.runner.PRODUCTION_UNIT}
        for case in cls.runner.CASES:
            sources.update(case.sources)
        files = set(sources)
        for source in sources:
            result = subprocess.run(
                [cls.cxx, *cls.runner.PRODUCTION_FLAGS, "-I", str(ROOT / "src"), "-I",
                 str(ROOT / "tests/linux_free_camera"), "-MM", str(ROOT / source)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True)
            for token in result.stdout.replace("\\\n", " ").split()[1:]:
                path = Path(token).resolve()
                files.add(str(path.relative_to(ROOT)))
        cls.tree_files = sorted(files)

    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="free-camera-runner-test-")
        self.repo = Path(self._temporary.name)
        for relative in self.tree_files:
            destination = self.repo / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)

    def tearDown(self):
        self._temporary.cleanup()

    def patch(self, relative, old, new):
        path = self.repo / relative
        text = path.read_text()
        if old not in text:
            self.skipTest(f"{relative} no longer contains the anchor this test inverts: {old!r}")
        path.write_text(text.replace(old, new, 1))

    def run_copy(self, *arguments):
        return run_runner(["--repo-root", self.repo, *arguments], timeout=900)

    def test_a_passing_copy_exits_zero(self):
        result = self.run_copy("--only", "free_camera")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("all 2 check(s) passed", result.stdout)

    def test_a_behaviour_change_in_the_owner_fails(self):
        # Let a lost device draw: the recording renderer must catch it.
        self.patch("src/linux/client/view/frame_owner.cpp",
                   "            state_ = FrameState::DeviceLost;\n        return FrameOutcome::DeviceLost;\n    case",
                   "            state_ = FrameState::DeviceLost;\n        break;\n    case")
        result = self.run_copy("--only", "frame_owner")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("1 of 2 check(s) failed", result.stdout)

    def test_a_reported_failure_with_a_zero_exit_still_fails(self):
        (self.repo / "tests/linux_free_camera/free_camera_test.cpp").write_text(REPORTS_FAILURE_AND_EXITS_ZERO)
        result = self.run_copy("--only", "free_camera")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failure line(s) reported", result.stdout)

    def test_a_warning_in_new_code_is_a_compile_failure(self):
        self.patch("src/linux/client/view/free_camera_controls.cpp",
                   "bool FreeCameraControls::Key(int key, bool down)\n{\n",
                   "bool FreeCameraControls::Key(int key, bool down)\n{\n    int unused_by_contract_test;\n")
        result = self.run_copy("--only", "free_camera")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("compile failed", result.stdout)

    def test_a_stuck_test_is_killed(self):
        (self.repo / "tests/linux_free_camera/free_camera_test.cpp").write_text(STUCK)
        result = self.run_copy("--only", "free_camera", "--timeout", "2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TIMEOUT", result.stdout)

    def test_a_missing_production_unit_is_reported_before_compiling(self):
        (self.repo / "src/universal/com_math_anglevectors.cpp").unlink()
        result = self.run_copy("--only", "free_camera")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source is missing", result.stdout)
        self.assertIn("com_math_anglevectors.cpp", result.stdout)
        self.assertNotIn("$ ", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
