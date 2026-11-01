#!/usr/bin/env python3
"""Contract tests for run_world_surface_checks.py.

These tests check the runner itself, not the world surface code: what it compiles,
with which flags, and whether a failed compile, a failed check, a reported failure
with a zero exit, a stuck check and a missing source each reach the caller as a
non-zero exit. The negative cases run against a temporary copy of the sources with
one deliberate defect, so the real check files stay untouched.

The positive side is not re-asserted here: the runner is what runs the world
surface checks, and a hosted pass is only claimed once CI has actually run it.
"""

from importlib import util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "tests" / "run_world_surface_checks.py"

# The smallest tree the compiled checks need: the six test headers, the six test
# sources, the one src header the inventory test includes, and the one src unit the
# capture test links against.
TREE_FILES = (
    "tests/linux_asset_export/world_surface_inventory_test.cpp",
    "tests/linux_renderer/world_surface_context_test.cpp",
    "tests/linux_renderer/world_surface_context.h",
    "tests/linux_renderer/world_surface_bundle_test.cpp",
    "tests/linux_renderer/world_surface_bundle.h",
    "tests/linux_renderer/world_surface_oracle_test.cpp",
    "tests/linux_renderer/world_surface_oracle.h",
    "tests/linux_renderer/world_lit_bundle_test.cpp",
    "tests/linux_renderer/world_lit_bundle.h",
    "src/linux/tools/world_surface_export.h",
    "tests/linux_asset_export/world_image_capture_test.cpp",
    "src/linux/tools/world_image_capture.h",
    "src/linux/tools/world_image_capture.cpp",
)

# The compiled checks, in the runner's order.
CASE_NAMES = [
    "world_surface_inventory",
    "world_surface_context",
    "world_surface_bundle",
    "world_surface_oracle",
    "world_lit_bundle",
    "world_image_capture",
]

# A program that reports a failure and still exits 0. A runner that trusted only
# the exit status would call this a pass.
REPORTS_FAILURE_AND_EXITS_ZERO = """\
#include <cstdio>
int main()
{
    std::printf("FAIL forced by the runner contract test\\n");
    return 0;
}
"""

# A program that never returns on its own, so the runner's timeout has to end it.
STUCK = """\
#include <unistd.h>
int main()
{
    sleep(120);
    return 0;
}
"""


def load_runner():
    """Import the runner as a module without running it."""
    spec = util.spec_from_file_location("run_world_surface_checks", RUNNER)
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Options:
    """The subset of parsed arguments plan_checks reads."""

    def __init__(self, config="Debug", timeout=300, compile_timeout=600, cxx_flag=()):
        self.config = config
        self.timeout = timeout
        self.compile_timeout = compile_timeout
        self.cxx_flag = list(cxx_flag)


class RunnerPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner()

    def test_configurations_are_exactly_two_and_state_their_own_optimization(self):
        # The workflow matrix and this table have to agree: a configuration named
        # here without a flag would silently compile at the compiler default.
        self.assertEqual(sorted(self.runner.CONFIG_FLAGS), ["Debug", "RelWithDebInfo"])
        self.assertEqual(self.runner.CONFIG_FLAGS["Debug"], ("-O0",))
        self.assertEqual(self.runner.CONFIG_FLAGS["RelWithDebInfo"], ("-O2",))

    def test_every_case_points_at_a_source_that_exists(self):
        names = [case.name for case in self.runner.CASES]
        self.assertEqual(len(names), len(set(names)), "case names are unique")
        self.assertEqual(names, CASE_NAMES)
        for case in self.runner.CASES:
            with self.subTest(case=case.name):
                self.assertTrue((ROOT / case.source).is_file(), case.source)
                # Every extra translation unit is checked the same way: the runner
                # refusing a missing file has to name a file that is really there.
                for source in case.extra_sources:
                    self.assertTrue((ROOT / source).is_file(), source)

        cases = {case.name: case for case in self.runner.CASES}
        capture = cases["world_image_capture"]
        self.assertEqual(capture.extra_sources, ("src/linux/tools/world_image_capture.cpp",))
        # Nothing else in the table gained a second unit, so the compile lines of the
        # five single-unit renderer checks are unchanged.
        for name in CASE_NAMES[:-1]:
            self.assertEqual(cases[name].extra_sources, (), name)
        # The capture unit is one file from src/linux/tools. An engine source here
        # would make the check need an asset tree to link, which is exactly what this
        # runner exists to avoid.
        for case in self.runner.CASES:
            for source in case.sources:
                self.assertNotIn("src/linux/server_assets.cpp", source)
                self.assertNotIn("src/gfx_d3d/", source)
                self.assertNotIn("src/database/", source)

    def test_include_roots_are_the_ones_each_source_needs(self):
        cases = {case.name: case for case in self.runner.CASES}
        # The inventory test includes <linux/tools/world_surface_export.h> from src.
        self.assertEqual(cases["world_surface_inventory"].include_dirs, ("src",))
        # The capture test includes <linux/tools/world_image_capture.h> from src too.
        self.assertEqual(cases["world_image_capture"].include_dirs, ("src",))
        # The renderer tests include their own header beside them.
        for name in ("world_surface_context", "world_surface_bundle", "world_surface_oracle",
                     "world_lit_bundle"):
            self.assertEqual(cases[name].include_dirs, ("tests/linux_renderer",))

    def test_prepare_step_is_the_asset_free_prepare_test(self):
        self.assertEqual(self.runner.PREPARE_SCRIPT, "scripts/tests/test_world_surface_prepare.py")
        self.assertTrue((ROOT / self.runner.PREPARE_SCRIPT).is_file())
        self.assertIn(self.runner.PREPARE_NAME, self.runner.KNOWN_NAMES)

    def test_compile_line_is_32_bit_cxx20_and_needs_no_graphics_stack(self):
        for config in ("Debug", "RelWithDebInfo"):
            with self.subTest(config=config):
                argv = self.runner.compile_argv(
                    self.runner.CASES[0], ROOT, Path("/tmp/work"), "g++", config, [])
                self.assertIn("-m32", argv)
                self.assertIn("-std=c++20", argv)
                self.assertIn("-g", argv)
                self.assertIn("-Wall", argv)
                self.assertIn("-Wextra", argv)
                self.assertIn("-Werror", argv)
                self.assertIn(self.runner.CONFIG_FLAGS[config][0], argv)
                # The compile line is bare: no CMake, no pkg-config, no graphics
                # library, and no link step beyond the one source.
                for token in argv:
                    lowered = token.lower()
                    for forbidden in ("dxvk", "sdl", "vulkan", "cmake", "pkg-config", "pkgconfig"):
                        self.assertNotIn(forbidden, lowered)
                source = str(ROOT / self.runner.CASES[0].source)
                objects = [token for token in argv if token.endswith(".cpp")]
                self.assertEqual(objects, [source])

        # The capture case is the one line that is not a single translation unit: the
        # mutex in its unit needs -pthread, and the link needs the unit itself, or the
        # test's symbols stay undefined. Both are this case's own entry, nothing shared.
        cases = {case.name: case for case in self.runner.CASES}
        capture = cases["world_image_capture"]
        argv = self.runner.compile_argv(capture, ROOT, Path("/tmp/work"), "g++", "Debug", [])
        self.assertIn("-pthread", argv)
        self.assertEqual(
            [token for token in argv if token.endswith(".cpp")],
            [str(ROOT / capture.source), str(ROOT / capture.extra_sources[0])],
        )
        self.assertIn("-m32", argv)
        self.assertIn("-std=c++20", argv)
        self.assertIn("-Werror", argv)
        # No engine source and no library: the unit is engine-free by construction.
        for token in argv:
            lowered = token.lower()
            for forbidden in ("dxvk", "sdl", "vulkan", "cmake", "pkg-config", "pkgconfig",
                              "libengine", "server_assets"):
                self.assertNotIn(forbidden, lowered)
        # The flag is the capture case's alone, so the other compile lines are exactly
        # what they were before that case existed.
        for name in CASE_NAMES[:-1]:
            with self.subTest(case=name):
                self.assertEqual(cases[name].flags, ())
                self.assertNotIn(
                    "-pthread",
                    self.runner.compile_argv(cases[name], ROOT, Path("/tmp/work"), "g++", "Debug", []),
                )

    def test_the_two_configurations_differ_only_in_optimization(self):
        debug = self.runner.compile_argv(
            self.runner.CASES[0], ROOT, Path("/tmp/work"), "g++", "Debug", [])
        release = self.runner.compile_argv(
            self.runner.CASES[0], ROOT, Path("/tmp/work"), "g++", "RelWithDebInfo", [])
        self.assertEqual(len(debug), len(release))
        differences = [(a, b) for a, b in zip(debug, release) if a != b]
        self.assertEqual(differences, [("-O0", "-O2")])

    def test_extra_flags_are_only_what_the_caller_asked_for(self):
        # ASan and friends are opt-in: the default line carries none.
        plain = self.runner.compile_argv(
            self.runner.CASES[0], ROOT, Path("/tmp/work"), "g++", "Debug", [])
        self.assertFalse(any("sanitize" in token for token in plain))
        with_flag = self.runner.compile_argv(
            self.runner.CASES[0], ROOT, Path("/tmp/work"), "g++", "Debug",
            ["-fsanitize=address"])
        self.assertIn("-fsanitize=address", with_flag)

    def test_plan_is_one_compile_then_one_run_per_case_cplus_prepare(self):
        work = Path("/tmp/plan-work")
        checks = self.runner.plan_checks(
            Options(), work, ROOT, "/usr/bin/python3", "g++", set(self.runner.KNOWN_NAMES))
        # Six compiled cases plus the prepare script: every name in the table, in order.
        self.assertEqual(len(checks), 7)
        self.assertEqual([check.name for check in checks], [*CASE_NAMES, self.runner.PREPARE_NAME])
        for check in checks[:-1]:
            self.assertEqual(len(check.commands), 2, check.name)
            compile_step, run_step = check.commands
            self.assertIn("-m32", compile_step.argv)
            self.assertEqual(run_step.argv, [str(work / check.name)])
            self.assertEqual(check.binary, work / check.name)
        # The capture check runs the same one-compile-then-run shape as the others.
        capture = checks[CASE_NAMES.index("world_image_capture")]
        self.assertEqual(
            [token for token in capture.commands[0].argv if token.endswith(".cpp")],
            [str(ROOT / "tests/linux_asset_export/world_image_capture_test.cpp"),
             str(ROOT / "src/linux/tools/world_image_capture.cpp")],
        )
        prepare = checks[-1]
        self.assertEqual(len(prepare.commands), 1)
        self.assertEqual(prepare.commands[0].argv[1], str(ROOT / self.runner.PREPARE_SCRIPT))
        self.assertIsNone(prepare.binary)

    def test_an_unknown_check_name_lists_the_known_ones(self):
        result = run_runner(["--only", "not_a_check"])
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown check", result.stderr)
        for name in (*CASE_NAMES, "world_surface_prepare"):
            self.assertIn(name, result.stderr)

    def test_a_wrong_timeout_is_refused(self):
        self.assertEqual(run_runner(["--timeout", "0"]).returncode, 2)
        self.assertEqual(run_runner(["--compile-timeout", "0"]).returncode, 2)

    def test_list_prints_the_commands_and_runs_nothing(self):
        result = run_runner(["--config", "RelWithDebInfo", "--list"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--list: printed the command list, ran nothing.", result.stdout)
        self.assertIn("-O2", result.stdout)
        self.assertIn(str(ROOT / self.runner.CASES[2].source), result.stdout)
        self.assertNotIn("Summary", result.stdout)


class RunnerBehaviourTests(unittest.TestCase):
    """Each test drives the real script, at least one compile and one run."""

    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner()

    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="world-surface-runner-test-")
        self.repo = Path(self._temporary.name)
        for relative in TREE_FILES:
            destination = self.repo / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)

    def tearDown(self):
        self._temporary.cleanup()

    def patch(self, relative, old, new):
        path = self.repo / relative
        text = path.read_text()
        if old not in text:
            # The anchor is a line of a check this test does not own. If that line
            # is reshaped, say so rather than passing without having inverted
            # anything.
            self.skipTest(f"{relative} no longer contains the anchor this test inverts: {old!r}")
        path.write_text(text.replace(old, new, 1))

    def replace(self, relative, text):
        (self.repo / relative).write_text(text)

    def test_a_passing_copy_exits_zero(self):
        # The baseline: the temporary tree passes, so every failure below is the
        # defect and not the harness.
        result = run_runner(["--repo-root", self.repo, "--only", "world_surface_inventory"],
                            timeout=900)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("all 1 check(s) passed", result.stdout)
        self.assertIn("-m32", result.stdout)

    def test_a_failing_check_is_a_matching_nonzero_exit(self):
        # Invert one allowlist expectation: the test counts a failure and exits 1.
        self.patch("tests/linux_asset_export/world_surface_inventory_test.cpp",
                   'Check(IsAllowedMapName("mp_crash"), "mp_crash is allowlisted");',
                   'Check(!IsAllowedMapName("mp_crash"), "mp_crash is allowlisted");')
        result = run_runner(["--repo-root", self.repo, "--only", "world_surface_inventory"],
                            timeout=900)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("1 of 1 check(s) failed", result.stdout)
        self.assertIn("FAIL", result.stdout)
        self.assertNotIn("all 1 check(s) passed", result.stdout)

    def test_a_reported_failure_with_a_zero_exit_still_fails(self):
        self.replace("tests/linux_asset_export/world_surface_inventory_test.cpp",
                     REPORTS_FAILURE_AND_EXITS_ZERO)
        result = run_runner(["--repo-root", self.repo, "--only", "world_surface_inventory"],
                            timeout=900)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failure line(s) reported", result.stdout)
        self.assertNotIn("all 1 check(s) passed", result.stdout)

    def test_a_compile_error_stops_the_run_before_the_test(self):
        self.replace("tests/linux_renderer/world_surface_context_test.cpp",
                     '#error forced by the runner contract test\n')
        result = run_runner(["--repo-root", self.repo, "--only", "world_surface_context"],
                            timeout=900)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("forced by the runner contract test", result.stdout)
        self.assertIn("compile failed", result.stdout)
        self.assertNotIn("all 1 check(s) passed", result.stdout)

    def test_a_stuck_check_is_killed_at_the_timeout(self):
        self.replace("tests/linux_asset_export/world_surface_inventory_test.cpp", STUCK)
        result = run_runner(["--repo-root", self.repo, "--only", "world_surface_inventory",
                             "--timeout", "2"], timeout=900)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TIMEOUT", result.stdout)
        self.assertNotIn("all 1 check(s) passed", result.stdout)

    def test_a_missing_source_is_reported_before_compiling(self):
        (self.repo / "tests/linux_renderer/world_surface_context_test.cpp").unlink()
        result = run_runner(["--repo-root", self.repo, "--only", "world_surface_context"],
                            timeout=900)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source is missing", result.stdout)

        # The capture check compiles two files. Removing its unit has to be refused the
        # same way as removing a test: the compile would otherwise fail with an
        # undefined symbol instead of saying which file is gone.
        unit = self.repo / "src/linux/tools/world_image_capture.cpp"
        self.assertTrue(unit.is_file())
        unit.unlink()
        result = run_runner(["--repo-root", self.repo, "--only", "world_image_capture"],
                            timeout=900)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source is missing", result.stdout)
        self.assertIn("src/linux/tools/world_image_capture.cpp", result.stdout)

    def test_the_binary_check_tells_64_bit_from_32_bit(self):
        # The guard itself: this interpreter is a 64-bit ELF, and the runner's own
        # -m32 build is not.
        self.assertFalse(self.runner.is_target_elf(sys.executable))
        with tempfile.TemporaryDirectory(prefix="world-surface-elf-") as directory:
            binary = Path(directory) / "probe"
            compiled = subprocess.run(
                ["g++", "-m32", "-std=c++20", "-x", "c++", "-", "-o", str(binary)],
                input="int main(){return 0;}\n", text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            self.assertTrue(self.runner.is_target_elf(binary))
        # A file that is not an ELF at all, and a path that does not exist, are both
        # refused rather than raising.
        self.assertFalse(self.runner.is_target_elf(ROOT / "scripts/tests/README.md"))
        self.assertFalse(self.runner.is_target_elf(ROOT / "scripts/tests/no_such_file"))


def run_runner(arguments, timeout=120):
    return subprocess.run(
        [sys.executable, str(RUNNER), *[str(argument) for argument in arguments]],
        cwd=str(ROOT),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        timeout=timeout,
    )


if __name__ == "__main__":
    unittest.main()
