#!/usr/bin/env python3
"""Contract tests for run_native_d3d9_boundary_checks.py.

These test the runner, not the boundary: what it plans, with which flags, how it
reads a disassembly and compares layouts, how it stages DXVK headers, and that a
compile that should fail but passes, a rejection for the wrong reason, a check
that reports FAIL with exit 0, and a missing source each reach the caller as a
non-zero exit.

No DXVK, compiler or GPU is needed for most cases: subprocess behaviour is driven
with small Python programs, and DXVK trees are fabricated in a temporary
directory. The one end-to-end case compiles the stand-in header check with the
host's 32-bit g++ and is skipped (and says so) when that toolchain is missing.
A pass here does not claim the hosted workflow ran.
"""

from importlib import util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "tests" / "run_native_d3d9_boundary_checks.py"
CMAKE_MODULE = ROOT / "scripts" / "platform" / "linux" / "native_d3d9.cmake"


def load_runner():
    spec = util.spec_from_file_location("run_native_d3d9_boundary_checks", RUNNER)
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load_runner()


def options(**overrides):
    values = dict(
        config="Debug",
        header_cxx=["g++", "clang++"],
        engine_cxx="clang++",
        objdump="objdump",
        timeout=30,
        compile_timeout=60,
    )
    values.update(overrides)
    return types.SimpleNamespace(**values)


DISASSEMBLY = """\
00001200 <kisak_d3d9_probe_stdcall>:
    1200:\tpush   %ebp
    1201:\tmov    %esp,%ebp
    1210:\tret    $0x8

00001220 <kisak_d3d9_probe_cdecl>:
    1220:\tmov    0x8(%esp),%eax
    1224:\tret
"""


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.workdir = Path("/nonexistent-workdir")
        self.dxvk = Path("/nonexistent-dxvk/include/dxvk")
        self.checks = runner.plan_checks(options(), ROOT, self.workdir, self.dxvk)
        self.by_name = {check.name: check for check in self.checks}

    def test_both_header_compilers_cover_both_modes_and_every_rejection(self):
        for tag in ("gcc", "clang"):
            for name in ("boundary_standin", "boundary_native", "reject_direct_native_include",
                         "reject_d3d9_before_boundary", "reject_unfirewalled_dxvk",
                         "reject_standin_method_call"):
                with self.subTest(tag=tag, name=name):
                    self.assertIn(f"{name}-{tag}", self.by_name)

    def test_native_define_and_dxvk_path_only_where_native(self):
        for name, check in self.by_name.items():
            for step in check.steps:
                if step.kind not in ("compile", "reject"):
                    continue
                native = runner.NATIVE_DEFINE in step.argv
                with self.subTest(check=name):
                    self.assertEqual(native, str(self.dxvk) in step.argv)
                    if name.startswith("boundary_standin") or name == "engine_layout_standin" \
                            or name == "renderer_standin_control" or name.startswith("reject_standin"):
                        self.assertFalse(native)
                    if name.startswith(("boundary_native", "renderer_proof-", "reject_direct",
                                        "reject_d3d9_before", "reject_unfirewalled")) \
                            or name == "engine_layout_native":
                        self.assertTrue(native)

    def test_every_compile_is_32_bit_cxx20_and_boundary_sources_are_strict(self):
        for name, check in self.by_name.items():
            for step in check.steps:
                if step.kind not in ("compile", "reject"):
                    continue
                with self.subTest(check=name):
                    self.assertIn("-m32", step.argv)
                    self.assertIn("-std=c++20", step.argv)
                    self.assertIn("-O0", step.argv)
                    if name.startswith(("boundary_", "reject_")):
                        self.assertIn("-Werror", step.argv)

    def test_relwithdebinfo_is_o2(self):
        checks = runner.plan_checks(options(config="RelWithDebInfo"), ROOT, self.workdir, self.dxvk)
        compile_steps = [step for check in checks for step in check.steps if step.kind == "compile"]
        self.assertTrue(compile_steps)
        for step in compile_steps:
            self.assertIn("-O2", step.argv)
            self.assertNotIn("-O0", step.argv)
            self.assertNotIn("-D_DEBUG", step.argv)

    def test_engine_checks_use_the_engine_compiler_and_flags(self):
        for name in ("engine_layout_standin", "engine_layout_native", "renderer_proof-r_state"):
            argv = self.by_name[name].steps[0].argv
            with self.subTest(check=name):
                self.assertEqual(argv[0], "clang++")
                for flag in ("-DKISAK_MP", "-DKISAK_OPENAL", "-fms-extensions", "-msse2"):
                    self.assertIn(flag, argv)
                self.assertNotIn("-DKISAK_DEDICATED", argv)
                self.assertNotIn("-DDEDICATED", argv)

    def test_rejections_must_fail_for_their_stated_reason(self):
        for name, check in self.by_name.items():
            if not name.startswith("reject_"):
                continue
            with self.subTest(check=name):
                self.assertEqual(len(check.steps), 1)
                self.assertTrue(check.steps[0].expect_failure)
                self.assertIn("-fsyntax-only", check.steps[0].argv)

    def test_renderer_proof_matches_the_shared_list_and_the_cmake_module_reads_it(self):
        units = runner.read_proof_list(ROOT)
        self.assertGreater(len(units), 0)
        for unit in units:
            with self.subTest(unit=unit):
                self.assertTrue((ROOT / unit).is_file(), unit)
                self.assertTrue(unit.startswith("src/gfx_d3d/"), unit)
                self.assertIn(f"renderer_proof-{Path(unit).stem}", self.by_name)
        control = self.by_name["renderer_standin_control"]
        self.assertEqual(len(control.steps), len(units))
        self.assertTrue(all(step.expect_failure == "" for step in control.steps))
        self.assertIn("native_d3d9_renderer_sources.txt", CMAKE_MODULE.read_text())

    def test_layout_compare_runs_after_both_layout_builds(self):
        names = [check.name for check in self.checks]
        self.assertLess(names.index("engine_layout_standin"), names.index("engine_layout_compare"))
        self.assertLess(names.index("engine_layout_native"), names.index("engine_layout_compare"))


class DisassemblyTests(unittest.TestCase):
    def test_expected_returns_pass(self):
        problems = runner.check_conventions(DISASSEMBLY, {
            "kisak_d3d9_probe_stdcall": runner.STDCALL_RET,
            "kisak_d3d9_probe_cdecl": runner.CDECL_RET,
        })
        self.assertEqual(problems, [])

    def test_an_erased_stdcall_is_reported(self):
        erased = DISASSEMBLY.replace("ret    $0x8", "ret")
        problems = runner.check_conventions(erased, {"kisak_d3d9_probe_stdcall": runner.STDCALL_RET})
        self.assertEqual(len(problems), 1)
        self.assertIn("expected 'ret $0x8'", problems[0])

    def test_a_missing_probe_is_reported(self):
        problems = runner.check_conventions(DISASSEMBLY, {"_ZN10ProbeQuery5IssueEj": runner.CDECL_RET})
        self.assertEqual(problems, ["_ZN10ProbeQuery5IssueEj: not found in the disassembly"])

    def test_a_cdecl_method_that_pops_is_reported(self):
        popped = DISASSEMBLY.replace("    1224:\tret", "    1224:\tret    $0x4")
        problems = runner.check_conventions(popped, {"kisak_d3d9_probe_cdecl": runner.CDECL_RET})
        self.assertEqual(len(problems), 1)


class LayoutCompareTests(unittest.TestCase):
    BODY = "size GfxImage 36 align 4\noffset GfxImage.texture 4\nenum D3DFMT_DXT1 827611204\nPASS engine D3D9 layouts\n"

    def test_identical_bodies_pass_whatever_the_mode_line(self):
        ok, detail = runner.compare_layouts("# a (stand-in view)\n" + self.BODY, "# a (native view)\n" + self.BODY)
        self.assertTrue(ok, detail)

    def test_a_changed_offset_fails(self):
        native = self.BODY.replace("GfxImage.texture 4", "GfxImage.texture 8")
        ok, detail = runner.compare_layouts("# s\n" + self.BODY, "# n\n" + native)
        self.assertFalse(ok)
        self.assertIn("GfxImage.texture", detail)

    def test_an_extra_line_fails(self):
        native = self.BODY.replace("PASS", "size Extra 4 align 4\nPASS")
        ok, _ = runner.compare_layouts("# s\n" + self.BODY, "# n\n" + native)
        self.assertFalse(ok)

    def test_an_unfinished_print_fails(self):
        ok, _ = runner.compare_layouts("# s\n" + self.BODY, "# n\nsize GfxImage 36 align 4\n")
        self.assertFalse(ok)


class DxvkStagingTests(unittest.TestCase):
    def make_source(self, root, version="2.7.1", with_directx=True):
        native = root / "include" / "native"
        (native / "windows").mkdir(parents=True)
        (native / "wsi").mkdir(parents=True)
        (native / "directx").mkdir(parents=True)
        (root / "meson.build").write_text(
            f"project('dxvk', ['c', 'cpp'], version : '{version}', meson_version : '>= 0.58')\n")
        for name in ("windows.h", "windows_base.h", "unknwn.h"):
            (native / "windows" / name).write_text("/* fake */\n")
        for name in runner.DXVK_WSI_HEADERS:
            (native / "wsi" / name).write_text("/* fake */\n")
        if with_directx:
            for name in ("d3d9.h", "d3d9types.h", "d3d9caps.h"):
                (native / "directx" / name).write_text("/* fake */\n")
            (native / "directx" / ".git").write_text("gitdir: elsewhere\n")

    def test_meson_layout_is_reproduced(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_source(root / "src")
            destination = root / "out" / "dxvk"
            self.assertEqual(runner.stage_dxvk_headers(root / "src", destination), [])
            self.assertTrue((destination / "d3d9.h").is_file())
            self.assertTrue((destination / "windows_base.h").is_file())
            self.assertTrue((destination / "wsi" / "native_sdl3.h").is_file())
            self.assertFalse((destination / ".git").exists())
            self.assertEqual(runner.dxvk_source_version(root / "src"), "2.7.1")

    def test_stage_only_mode_writes_the_layout_and_runs_nothing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_source(root / "src")
            destination = root / "staged" / "dxvk"
            result = subprocess.run(
                [sys.executable, str(RUNNER), "--dxvk-source", str(root / "src"),
                 "--stage-dxvk-headers", str(destination)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("staged DXVK 2.7.1", result.stdout)
            self.assertNotIn("check(s)", result.stdout)
            self.assertTrue((destination / "d3d9.h").is_file())
            refused = subprocess.run(
                [sys.executable, str(RUNNER), "--stage-dxvk-headers", str(destination)],
                capture_output=True, text=True, timeout=60)
            self.assertNotEqual(refused.returncode, 0)

    def test_missing_directx_submodule_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_source(root / "src", with_directx=False)
            problems = runner.stage_dxvk_headers(root / "src", root / "out")
            self.assertEqual(len(problems), 1)
            self.assertIn("submodule", problems[0])

    def test_wrong_version_fails_the_run_before_any_compile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_source(root / "src", version="2.6")
            result = subprocess.run(
                [sys.executable, str(RUNNER), "--dxvk-source", str(root / "src"), "--list"],
                capture_output=True, text=True, timeout=60)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("expected '2.7.1'", result.stdout)

    def test_an_include_dir_without_the_headers_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = subprocess.run(
                [sys.executable, str(RUNNER), "--dxvk-include-dir", temporary, "--list"],
                capture_output=True, text=True, timeout=60)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("has no d3d9.h", result.stdout)

    def test_no_dxvk_argument_fails_unless_listing(self):
        result = subprocess.run([sys.executable, str(RUNNER)], capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--dxvk-source or --dxvk-include-dir", result.stdout)
        listing = subprocess.run([sys.executable, str(RUNNER), "--list"], capture_output=True, text=True, timeout=60)
        self.assertEqual(listing.returncode, 0, listing.stdout)
        self.assertIn("ran nothing", listing.stdout)


class StepOutcomeTests(unittest.TestCase):
    """Exit-status handling, driven with tiny Python programs instead of compilers."""

    def step(self, code, expect_failure=None, kind="run"):
        return runner.Step([sys.executable, "-c", code], ROOT, 30, kind, expect_failure=expect_failure)

    def run_check(self, steps, verify=None):
        check = runner.Check("probe", "probe", steps, verify=verify)
        with tempfile.TemporaryDirectory() as logs:
            results, failures = runner.run_checks([check], logs)
            self.assertTrue((Path(logs) / "summary.txt").is_file())
        return results[0][1], failures

    def test_success_passes(self):
        self.assertEqual(self.run_check([self.step("print('ok')")]), ("ok", 0))

    def test_nonzero_exit_fails(self):
        self.assertEqual(self.run_check([self.step("raise SystemExit(3)")])[0], "failed")

    def test_reported_fail_with_exit_zero_fails(self):
        self.assertEqual(self.run_check([self.step("print('FAIL forced')")])[0], "failed")

    def test_expected_failure_that_succeeds_fails(self):
        status, _ = self.run_check([self.step("print('compiled')", expect_failure="", kind="reject")])
        self.assertEqual(status, "failed")

    def test_expected_failure_for_the_wrong_reason_fails(self):
        code = "import sys; print('error: something else'); sys.exit(1)"
        status, _ = self.run_check([self.step(code, expect_failure=r"single D3D9 entry point", kind="reject")])
        self.assertEqual(status, "failed")

    def test_expected_failure_for_the_right_reason_passes(self):
        code = "import sys; print('error: the single D3D9 entry point'); sys.exit(1)"
        status, _ = self.run_check([self.step(code, expect_failure=r"single D3D9 entry point", kind="reject")])
        self.assertEqual(status, "ok")

    def test_timeout_fails(self):
        check = runner.Check("slow", "slow", [runner.Step([sys.executable, "-c", "import time; time.sleep(30)"],
                                                           ROOT, 1, "run")])
        results, failures = runner.run_checks([check], None)
        self.assertEqual(results[0][1], "timeout")
        self.assertEqual(failures, 1)

    def test_a_failing_verifier_fails_the_check(self):
        status, _ = self.run_check([self.step("print('ok')")], verify=lambda outputs, results: (False, "nope"))
        self.assertEqual(status, "failed")


class EndToEndTests(unittest.TestCase):
    """One real compile: the stand-in header check, which needs no DXVK header."""

    @classmethod
    def setUpClass(cls):
        cls.available = bool(shutil.which("g++") and shutil.which("objdump"))
        if cls.available:
            with tempfile.TemporaryDirectory() as temporary:
                probe = Path(temporary) / "probe.cpp"
                probe.write_text("int main() { return 0; }\n")
                result = subprocess.run(["g++", "-m32", "-std=c++20", str(probe), "-o", str(Path(temporary) / "p")],
                                        capture_output=True, timeout=120)
                cls.available = result.returncode == 0

    def make_tree(self, root, break_check=False):
        for relative in ("src/linux/compat.h", "src/linux/d3d9_types.h", "src/linux/d3d9_native.h",
                         "src/linux/sys_fileio.h", "src/linux/sys_virtualmem.h",
                         runner.HEADER_CHECK, runner.LAYOUT_CHECK, runner.PROOF_LIST,
                         *(case[1] for case in runner.REJECT_CASES)):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        for unit in runner.read_proof_list(ROOT):
            target = root / unit
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("// placeholder: not compiled by this test\n")
        if break_check:
            source = root / runner.HEADER_CHECK
            text = source.read_text()
            text = text.replace('check(cdeclFn(40, 2) == (40 ^ 2), "plain cdecl probe");',
                                'check(cdeclFn(40, 2) == 0, "plain cdecl probe (broken by the contract test)");')
            source.write_text(text)
        dxvk = root / "fake-dxvk"
        dxvk.mkdir()
        for name in runner.REQUIRED_DXVK_HEADERS:
            (dxvk / name).write_text("/* never included by the stand-in check */\n")
        return dxvk

    def run_runner(self, root, dxvk):
        return subprocess.run(
            [sys.executable, str(RUNNER), "--repo-root", str(root), "--dxvk-include-dir", str(dxvk),
             "--header-cxx", "g++", "--only", "boundary_standin-gcc"],
            capture_output=True, text=True, timeout=300)

    def test_stand_in_check_passes_and_a_broken_copy_fails(self):
        if not self.available:
            self.skipTest("g++ -m32 or objdump is not available on this host")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "good"
            dxvk = self.make_tree(root)
            result = self.run_runner(root, dxvk)
            self.assertEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("4 calling-convention probe(s) as expected", result.stdout)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "broken"
            dxvk = self.make_tree(root, break_check=True)
            result = self.run_runner(root, dxvk)
            self.assertNotEqual(result.returncode, 0, result.stdout[-4000:])
            self.assertIn("FAIL plain cdecl probe (broken by the contract test)", result.stdout)
            self.assertIn("1 of 1 check(s) failed", result.stdout)


if __name__ == "__main__":
    unittest.main()
