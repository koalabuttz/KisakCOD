#!/usr/bin/env python3
"""Asset-free tests for run_world_frontend_checks.py and its CI job.

The runner's failure handling is driven with a fake compiler: compiles succeed
or fail per source, and the "link" copies in a tiny real i386 program whose
output and exit status each test chooses. So every way a run can go wrong is
checked in seconds without compiling the engine. The DXVK headers are a directory
of empty files with the names the runner requires, since the fake compiler never
reads them. The source list and the configurations are checked against the
repository as it is.
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
import run_native_d3d9_boundary_checks as boundary  # noqa: E402
import run_world_frontend_checks as runner  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "build-kisakcod-linux.yaml"


def run_main(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = runner.main(argv)
    return status, out.getvalue()


def fake_dxvk_include(directory):
    """An include directory holding every header name the runner requires."""
    include = Path(directory) / "dxvk"
    include.mkdir(parents=True, exist_ok=True)
    for name in boundary.REQUIRED_DXVK_HEADERS:
        (include / name).write_text("", encoding="utf-8")
    return include


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

    def test_only_the_boundary_units_are_on_the_census_blocked_list(self):
        # The census compiles with the stand-in, so a unit that needs the D3D9 API
        # is blocked there and must be compiled through the boundary here.
        blocked = census.load_blocked(census.BLOCKED_LIST)
        self.assertEqual(set(runner.PRODUCTION_SOURCES) & set(blocked), set(runner.NATIVE_D3D9_SOURCES))

    def test_boundary_units_are_on_the_boundary_proof_list(self):
        proof = boundary.read_proof_list(ROOT)
        self.assertTrue(runner.NATIVE_D3D9_SOURCES)
        for source in runner.NATIVE_D3D9_SOURCES:
            with self.subTest(source=source):
                self.assertIn(source, proof)
                self.assertIn(source, runner.PRODUCTION_SOURCES)

    def test_linked_owners_lose_their_boundary_storage(self):
        # The boundary file zero-initializes storage only for owners that are not
        # linked; a linked owner and a boundary definition would both define it.
        text = (ROOT / runner.BOUNDARY_SOURCE).read_text(encoding="utf-8")
        for owner, definition in (("src/gfx_d3d/r_scene.cpp", "GfxScene scene;"),
                                  ("src/gfx_d3d/r_buffers.cpp", "GfxBuffers gfxBuf;")):
            with self.subTest(owner=owner):
                self.assertIn(owner, runner.PRODUCTION_SOURCES)
                self.assertNotIn(definition, text)

    def test_r_init_stays_out(self):
        # r_init.cpp owns dx, rgp and gfxCfg, which the boundary file defines; it
        # creates the window and the device, and this check makes neither.
        self.assertNotIn("src/gfx_d3d/r_init.cpp", runner.PRODUCTION_SOURCES)

    def test_a_boundary_unit_outside_the_production_list_is_rejected(self):
        original = runner.NATIVE_D3D9_SOURCES
        try:
            runner.NATIVE_D3D9_SOURCES = original + ("src/gfx_d3d/r_init.cpp",)
            problems = runner.check_sources()
        finally:
            runner.NATIVE_D3D9_SOURCES = original
        self.assertIn("src/gfx_d3d/r_init.cpp is compiled through the boundary but is not a production source", problems)

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
            argv = runner.compile_argv("cxx", source, "RelWithDebInfo", Path("out.o"), Path("dxvk"))
            with self.subTest(source=source):
                self.assertIn("-m32", argv)
                self.assertIn("-DRELEASE_ASSERTS", argv)
                self.assertIn("-DKISAK_MP", argv)
                self.assertNotIn("-DDEDICATED", argv)
                self.assertNotIn("-DKISAK_DEDICATED", argv)
                self.assertEqual(argv[-4:], ["-c", source, "-o", "out.o"])

    def test_the_test_units_compile_with_warnings_as_errors(self):
        for source in (runner.TEST_SOURCE, runner.BOUNDARY_SOURCE):
            argv = runner.compile_argv("cxx", source, "Debug", Path("o"), Path("dxvk"))
            self.assertIn("-Werror", argv)
            self.assertIn("-isystem", argv)

    def test_production_units_compile_unmodified_with_the_target_flags(self):
        argv = runner.compile_argv("cxx", runner.PRODUCTION_SOURCES[0], "Debug", Path("o"), Path("dxvk"))
        self.assertNotIn("-Werror", argv)
        for flag in ("-fms-extensions", "-msse2", "-mfpmath=sse", "-fno-strict-aliasing", "-ffunction-sections"):
            self.assertIn(flag, argv)
        self.assertIn("--gc-sections", " ".join(runner.LINK_FLAGS))

    def test_only_the_boundary_units_see_dxvk(self):
        for source in runner.all_sources():
            argv = runner.compile_argv("cxx", source, "Debug", Path("o"), Path("/dxvk/include"))
            native = source in runner.NATIVE_D3D9_SOURCES
            with self.subTest(source=source):
                self.assertEqual(boundary.NATIVE_DEFINE in argv, native)
                self.assertEqual("/dxvk/include" in argv, native)
                if native:
                    self.assertEqual(argv[argv.index("/dxvk/include") - 1], "-isystem")

    def test_list_prints_every_line_and_runs_nothing(self):
        status, output = run_main(["--list", "--cxx", "/nonexistent/cxx", "--config", "RelWithDebInfo"])
        self.assertEqual(status, 0)
        lines = output.strip().splitlines()
        self.assertEqual(len(lines), len(runner.all_sources()))
        self.assertTrue(all("-O2" in line for line in lines))
        self.assertEqual(sum("<dxvk-include-dir>" in line for line in lines), len(runner.NATIVE_D3D9_SOURCES))


class DxvkHeaderTests(unittest.TestCase):
    """The DXVK headers are required and checked before anything compiles."""

    def setUp(self):
        self._dir = tempfile.TemporaryDirectory(prefix="kisak-frontend-dxvk-test-")
        self.addCleanup(self._dir.cleanup)
        self.dir = Path(self._dir.name)

    def test_no_dxvk_headers_fails_before_compiling(self):
        status, output = run_main(["--cxx", "/nonexistent/cxx"])
        self.assertEqual(status, 1)
        self.assertIn("pass --dxvk-source or --dxvk-include-dir", output)
        self.assertNotIn("could not start", output)

    def test_an_incomplete_include_dir_fails_before_compiling(self):
        include = fake_dxvk_include(self.dir)
        (include / "d3d9.h").unlink()
        status, output = run_main(["--cxx", "/nonexistent/cxx", "--dxvk-include-dir", str(include)])
        self.assertEqual(status, 1)
        self.assertIn("has no d3d9.h", output)
        self.assertNotIn("could not start", output)

    def test_a_source_tree_of_another_version_fails(self):
        source = self.dir / "dxvk-src"
        source.mkdir()
        (source / "meson.build").write_text("project('dxvk', ['c', 'cpp'], version : '2.6')\n", encoding="utf-8")
        status, output = run_main(["--cxx", "/nonexistent/cxx", "--dxvk-source", str(source)])
        self.assertEqual(status, 1)
        self.assertIn("DXVK source version is '2.6', expected '2.7.1'", output)

    def test_both_header_options_are_rejected(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            runner.main(["--dxvk-source", "a", "--dxvk-include-dir", "b"])


@unittest.skipUnless(m32_cxx(), "a compiler that links -m32 is required for the fake-run tests")
class FakeRunTests(unittest.TestCase):
    """End-to-end runs of main() with a fake compiler and a chosen test program."""

    def setUp(self):
        self._dir = tempfile.TemporaryDirectory(prefix="kisak-frontend-runner-test-")
        self.addCleanup(self._dir.cleanup)
        self.dir = Path(self._dir.name)
        self.dxvk = ["--dxvk-include-dir", str(fake_dxvk_include(self.dir))]

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
        status, output = run_main(["--cxx", cxx, "--config", "Debug", *self.dxvk])
        self.assertEqual(status, 0, output)
        self.assertIn("PASS: world front-end checks (Debug)", output)

    def test_a_reported_failure_fails_even_with_exit_zero(self):
        cxx = self.fake_cxx(self.program("FAIL: something\nPASS: world front end\n"))
        status, output = run_main(["--cxx", cxx, *self.dxvk])
        self.assertEqual(status, 1)
        self.assertIn("FAIL: world front-end test", output)

    def test_a_missing_pass_line_fails(self):
        cxx = self.fake_cxx(self.program("26 checks\n"))
        status, _ = run_main(["--cxx", cxx, *self.dxvk])
        self.assertEqual(status, 1)

    def test_a_nonzero_exit_fails_even_with_the_pass_line(self):
        cxx = self.fake_cxx(self.program("PASS: world front end\n", exit_code=3))
        status, output = run_main(["--cxx", cxx, *self.dxvk])
        self.assertEqual(status, 1)
        self.assertIn("exit 3", output)

    def test_a_hung_program_is_killed_and_fails(self):
        cxx = self.fake_cxx(self.program("", hang=True))
        status, output = run_main(["--cxx", cxx, "--run-timeout", "2", *self.dxvk])
        self.assertEqual(status, 1)
        self.assertIn("timeout", output)

    def test_a_unit_that_does_not_compile_fails_and_is_logged(self):
        source = "src/gfx_d3d/r_dpvs.cpp"
        log_dir = self.dir / "logs"
        cxx = self.fake_cxx(self.program("PASS: world front end\n"), failing=[source])
        status, output = run_main(["--cxx", cxx, "--log-dir", str(log_dir), *self.dxvk])
        self.assertEqual(status, 1)
        self.assertIn(f"FAIL: compile {source}", output)
        self.assertIn("fake failure", (log_dir / "src_gfx_d3d_r_dpvs.cpp.log").read_text(encoding="utf-8"))

    def test_a_binary_that_is_not_elf32_i386_fails(self):
        script = self.dir / "not-elf"
        script.write_text("#!/bin/sh\necho 'PASS: world front end'\n", encoding="utf-8")
        script.chmod(0o755)
        status, output = run_main(["--cxx", self.fake_cxx(script), *self.dxvk])
        self.assertEqual(status, 1)
        self.assertIn("not an ELF32 i386 executable", output)

    def test_a_missing_compiler_fails(self):
        status, output = run_main(["--cxx", str(self.dir / "no-such-cxx"), *self.dxvk])
        self.assertEqual(status, 1)
        self.assertIn("could not start", output)


class WorkflowTests(unittest.TestCase):
    """The hosted job: toolchain, OpenAL and pinned DXVK headers; no graphics stack or assets."""

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(WORKFLOW.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        cls.job = cls.workflow["jobs"]["world_frontend"]
        cls.commands = "\n".join(step.get("run", "") for step in cls.job["steps"])

    def test_both_configurations_run(self):
        # One job runs both through scripts/ci/each-config.sh, so the pair needs
        # one runner; test_ci_each_config.py checks that either failure is kept.
        self.assertNotIn("strategy", self.job)
        run = next(step["run"] for step in self.job["steps"]
                   if step.get("name") == "Run world front-end checks")
        lines = run.splitlines()
        start = lines.index("scripts/ci/each-config.sh <<'COMMANDS'")
        end = lines.index("COMMANDS")
        self.assertIn('--log-dir "artifacts/world-frontend-$config"', "\n".join(lines[start:end]))
        self.assertEqual([line for line in lines[end + 1:] if line.strip()], [])

    def test_the_self_tests_run_before_the_checks(self):
        names = [step.get("name") for step in self.job["steps"]]
        self.assertLess(names.index("Check world front-end runner"), names.index("Run world front-end checks"))
        self.assertIn("python3 scripts/tests/test_world_frontend_check_runner.py", self.commands)
        self.assertIn('--config "$config"', self.commands)
        self.assertIn("--cxx clang++", self.commands)

    def test_every_command_block_propagates_failure(self):
        for step in self.job["steps"]:
            if "run" in step:
                with self.subTest(step=step.get("name")):
                    self.assertTrue(step["run"].lstrip().startswith("set -euo pipefail"))

    def test_the_job_needs_no_graphics_stack_assets_or_engine_build(self):
        for forbidden in ("sdl", "vulkan", "xvfb", "steam", "cmake --build", "build-linux.sh", "ninja", "meson setup",
                          "libdxvk", "--stage-dxvk-headers"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.commands.lower())
        install = next(step for step in self.job["steps"] if step.get("name") == "Install world front-end tools")
        for package in ("clang", "g++-multilib", "libopenal-dev"):
            self.assertIn(package, install["run"])

    def test_dxvk_headers_come_from_the_boundary_jobs_pins(self):
        boundary_job = self.workflow["jobs"]["native_d3d9_boundary"]
        for key in ("DXVK_REVISION", "DXVK_DIRECTX_HEADERS_REVISION"):
            with self.subTest(key=key):
                self.assertEqual(self.job["env"][key], boundary_job["env"][key])
        names = [step.get("name") for step in self.job["steps"]]
        fetch = names.index("Fetch pinned DXVK Native 2.7.1 headers")
        self.assertLess(fetch, names.index("Run world front-end checks"))
        fetch_run = self.job["steps"][fetch]["run"]
        boundary_fetch = next(step["run"] for step in boundary_job["steps"]
                              if step.get("name") == "Fetch pinned DXVK Native 2.7.1 headers")
        # The same verified fetch: both revisions are checked, and the version too.
        for line in ('test "$(git -C "$src" rev-parse HEAD)" = "$DXVK_REVISION"',
                     'test "$recorded" = "$DXVK_DIRECTX_HEADERS_REVISION"',
                     'test "$(git -C "$headers" rev-parse HEAD)" = "$DXVK_DIRECTX_HEADERS_REVISION"',
                     "grep -q \"version : '2.7.1'\" \"$src/meson.build\""):
            with self.subTest(line=line):
                self.assertIn(line, fetch_run)
                self.assertIn(line, boundary_fetch)
        self.assertIn('--dxvk-source "$RUNNER_TEMP/dxvk-source"', self.commands)

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
