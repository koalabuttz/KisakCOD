#!/usr/bin/env python3
"""Asset-free tests for run_world_frontend_checks.py and its CI job.

The runner's failure handling is driven with a fake compiler: compiles succeed
or fail per source, and the "link" copies in a tiny real i386 program whose
output and exit status each test chooses. So every way a run can go wrong is
checked in seconds without compiling the engine. The source list and the
configurations are checked against the repository as it is.
"""

import contextlib
import io
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_client_compile_census as census  # noqa: E402
import run_world_frontend_checks as runner  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "build-kisakcod-linux.yaml"


def run_main(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = runner.main(argv)
    return status, out.getvalue()


def m32_cxx():
    """A compiler that can link -m32, or None."""
    for cxx in ("clang++", "g++"):
        if shutil.which(cxx) is None:
            continue
        with tempfile.TemporaryDirectory() as workdir:
            source = Path(workdir) / "probe.cpp"
            source.write_text("int main() { return 0; }\n", encoding="utf-8")
            probe = subprocess.run([cxx, "-m32", str(source), "-o", str(Path(workdir) / "probe")],
                                   capture_output=True, check=False)
            if probe.returncode == 0:
                return cxx
    return None


class SourceListTests(unittest.TestCase):
    def test_every_listed_source_exists_once(self):
        self.assertEqual(runner.check_sources(), [])

    def test_the_test_and_its_boundary_come_first(self):
        self.assertEqual(runner.all_sources()[:2], (runner.TEST_SOURCE, runner.BOUNDARY_SOURCE))

    def test_production_units_are_engine_sources_not_test_code(self):
        for source in runner.PRODUCTION_SOURCES:
            with self.subTest(source=source):
                self.assertTrue(source.startswith("src/"))
                self.assertTrue(source.endswith(".cpp"))

    def test_no_production_unit_is_on_the_census_blocked_list(self):
        blocked = census.load_blocked(census.BLOCKED_LIST)
        self.assertFalse(set(runner.PRODUCTION_SOURCES) & set(blocked))

    def test_the_d3d9_units_stay_out_until_the_boundary_lands(self):
        # These own storage the boundary file zero-initializes; linking one of
        # them means the boundary file must lose that definition in the same change.
        for source in ("src/gfx_d3d/r_init.cpp", "src/gfx_d3d/r_scene.cpp", "src/gfx_d3d/r_buffers.cpp"):
            self.assertNotIn(source, runner.PRODUCTION_SOURCES)

    def test_a_missing_or_duplicate_source_fails_before_compiling(self):
        original = runner.PRODUCTION_SOURCES
        try:
            runner.PRODUCTION_SOURCES = original + ("src/gfx_d3d/no_such_file.cpp", original[0])
            status, output = run_main(["--cxx", "/nonexistent/cxx"])
        finally:
            runner.PRODUCTION_SOURCES = original
        self.assertEqual(status, 1)
        self.assertIn("no_such_file.cpp does not exist", output)
        self.assertIn(f"{original[0]} is listed twice", output)


class CompileLineTests(unittest.TestCase):
    def test_configurations_are_explicit(self):
        self.assertEqual(set(runner.CONFIG_FLAGS), {"Debug", "RelWithDebInfo"})
        self.assertIn("-O0", runner.CONFIG_FLAGS["Debug"])
        self.assertIn("-O2", runner.CONFIG_FLAGS["RelWithDebInfo"])

    def test_every_unit_is_32_bit_with_live_assertions_and_client_defines(self):
        for source in runner.all_sources():
            argv = runner.compile_argv("cxx", source, "RelWithDebInfo", Path("out.o"))
            with self.subTest(source=source):
                self.assertIn("-m32", argv)
                self.assertIn("-DRELEASE_ASSERTS", argv)
                self.assertIn("-DKISAK_MP", argv)
                self.assertNotIn("-DDEDICATED", argv)
                self.assertNotIn("-DKISAK_DEDICATED", argv)
                self.assertEqual(argv[-4:], ["-c", source, "-o", "out.o"])

    def test_the_test_units_compile_with_warnings_as_errors(self):
        for source in (runner.TEST_SOURCE, runner.BOUNDARY_SOURCE):
            argv = runner.compile_argv("cxx", source, "Debug", Path("o"))
            self.assertIn("-Werror", argv)
            self.assertIn("-isystem", argv)

    def test_production_units_compile_unmodified_with_the_target_flags(self):
        argv = runner.compile_argv("cxx", runner.PRODUCTION_SOURCES[0], "Debug", Path("o"))
        self.assertNotIn("-Werror", argv)
        for flag in ("-fms-extensions", "-msse2", "-mfpmath=sse", "-fno-strict-aliasing", "-ffunction-sections"):
            self.assertIn(flag, argv)
        self.assertIn("--gc-sections", " ".join(runner.LINK_FLAGS))

    def test_list_prints_every_line_and_runs_nothing(self):
        status, output = run_main(["--list", "--cxx", "/nonexistent/cxx", "--config", "RelWithDebInfo"])
        self.assertEqual(status, 0)
        lines = output.strip().splitlines()
        self.assertEqual(len(lines), len(runner.all_sources()))
        self.assertTrue(all("-O2" in line for line in lines))


@unittest.skipUnless(m32_cxx(), "a compiler that links -m32 is required for the fake-run tests")
class FakeRunTests(unittest.TestCase):
    """End-to-end runs of main() with a fake compiler and a chosen test program."""

    def setUp(self):
        self._dir = tempfile.TemporaryDirectory(prefix="kisak-frontend-runner-test-")
        self.addCleanup(self._dir.cleanup)
        self.dir = Path(self._dir.name)

    def program(self, stdout, exit_code=0, hang=False):
        """A real i386 executable that prints stdout and exits with exit_code."""
        source = self.dir / "program.cpp"
        literal = '"' + stdout.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
        body = "for (;;) {}" if hang else f"std::fputs({literal}, stdout); return {exit_code};"
        source.write_text("#include <cstdio>\nint main() { " + body + " }\n", encoding="utf-8")
        binary = self.dir / "program"
        subprocess.run([m32_cxx(), "-m32", str(source), "-o", str(binary)], check=True)
        return binary

    def fake_cxx(self, program, failing=()):
        lines = ["#!/bin/sh", 'out=""; prev=""; compile=0',
                 'for a; do [ "$prev" = "-o" ] && out="$a"; [ "$a" = "-c" ] && compile=1; prev="$a"; done',
                 'if [ "$compile" = 1 ]; then']
        for source in failing:
            lines.append(f'  case "$*" in *" {source} "*) echo "error: fake failure in {source}" >&2; exit 1;; esac')
        lines += ['  : > "$out"; exit 0', "fi", f'exec cp "{program}" "$out"']
        script = self.dir / "fake-cxx"
        script.write_text("\n".join(lines) + "\n", encoding="utf-8")
        script.chmod(script.stat().st_mode | stat.S_IXUSR)
        return str(script)

    def test_a_passing_program_passes(self):
        cxx = self.fake_cxx(self.program("PASS: a\n26 checks, 0 failures\nPASS: world front end\n"))
        status, output = run_main(["--cxx", cxx, "--config", "Debug"])
        self.assertEqual(status, 0, output)
        self.assertIn("PASS: world front-end checks (Debug)", output)

    def test_a_reported_failure_fails_even_with_exit_zero(self):
        cxx = self.fake_cxx(self.program("FAIL: something\nPASS: world front end\n"))
        status, output = run_main(["--cxx", cxx])
        self.assertEqual(status, 1)
        self.assertIn("FAIL: world front-end test", output)

    def test_a_missing_pass_line_fails(self):
        cxx = self.fake_cxx(self.program("26 checks\n"))
        status, _ = run_main(["--cxx", cxx])
        self.assertEqual(status, 1)

    def test_a_nonzero_exit_fails_even_with_the_pass_line(self):
        cxx = self.fake_cxx(self.program("PASS: world front end\n", exit_code=3))
        status, output = run_main(["--cxx", cxx])
        self.assertEqual(status, 1)
        self.assertIn("exit 3", output)

    def test_a_hung_program_is_killed_and_fails(self):
        cxx = self.fake_cxx(self.program("", hang=True))
        status, output = run_main(["--cxx", cxx, "--run-timeout", "2"])
        self.assertEqual(status, 1)
        self.assertIn("timeout", output)

    def test_a_unit_that_does_not_compile_fails_and_is_logged(self):
        source = "src/gfx_d3d/r_dpvs.cpp"
        log_dir = self.dir / "logs"
        cxx = self.fake_cxx(self.program("PASS: world front end\n"), failing=[source])
        status, output = run_main(["--cxx", cxx, "--log-dir", str(log_dir)])
        self.assertEqual(status, 1)
        self.assertIn(f"FAIL: compile {source}", output)
        self.assertIn("fake failure", (log_dir / "src_gfx_d3d_r_dpvs.cpp.log").read_text(encoding="utf-8"))

    def test_a_binary_that_is_not_elf32_i386_fails(self):
        script = self.dir / "not-elf"
        script.write_text("#!/bin/sh\necho 'PASS: world front end'\n", encoding="utf-8")
        script.chmod(0o755)
        status, output = run_main(["--cxx", self.fake_cxx(script)])
        self.assertEqual(status, 1)
        self.assertIn("not an ELF32 i386 executable", output)

    def test_a_missing_compiler_fails(self):
        status, output = run_main(["--cxx", str(self.dir / "no-such-cxx")])
        self.assertEqual(status, 1)
        self.assertIn("could not start", output)


class WorkflowTests(unittest.TestCase):
    """The hosted job: toolchain plus OpenAL headers; no graphics stack or assets."""

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(WORKFLOW.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        cls.job = cls.workflow["jobs"]["world_frontend"]
        cls.commands = "\n".join(step.get("run", "") for step in cls.job["steps"])

    def test_both_configurations_run(self):
        self.assertEqual(self.job["strategy"]["matrix"]["config"], ["Debug", "RelWithDebInfo"])
        self.assertEqual(self.job["strategy"]["fail-fast"], "false")

    def test_the_self_tests_run_before_the_checks(self):
        names = [step.get("name") for step in self.job["steps"]]
        self.assertLess(names.index("Check world front-end runner"), names.index("Run world front-end checks"))
        self.assertIn("python3 scripts/tests/test_world_frontend_check_runner.py", self.commands)
        self.assertIn('--config "${{ matrix.config }}"', self.commands)
        self.assertIn("--cxx clang++", self.commands)

    def test_every_command_block_propagates_failure(self):
        for step in self.job["steps"]:
            if "run" in step:
                with self.subTest(step=step.get("name")):
                    self.assertTrue(step["run"].lstrip().startswith("set -euo pipefail"))

    def test_the_job_needs_no_graphics_stack_assets_or_engine_build(self):
        for forbidden in ("dxvk", "sdl", "vulkan", "xvfb", "steam", "cmake --build", "build-linux.sh", "ninja"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.commands.lower())
        install = next(step for step in self.job["steps"] if step.get("name") == "Install world front-end tools")
        for package in ("clang", "g++-multilib", "libopenal-dev"):
            self.assertIn(package, install["run"])

    def test_logs_are_kept_on_failure(self):
        upload = next(step for step in self.job["steps"]
                      if step.get("uses", "").startswith("actions/upload-artifact@"))
        self.assertEqual(upload["if"], "always()")

    def test_test_sources_trigger_the_workflow(self):
        triggers = self.workflow.get("on") or self.workflow.get(True)
        for event in ("push", "pull_request"):
            self.assertIn("tests/linux_world_frontend/**", triggers[event]["paths"])


if __name__ == "__main__":
    unittest.main()
