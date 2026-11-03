#!/usr/bin/env python3
"""Asset-free tests for run_x64_compile_census.py, its CMake knob and its CI job.

The census itself compiles ~600 units; these tests drive it with a fake compiler
(a shell script that prints chosen diagnostics) and a hand-written
compile_commands.json, so command rewriting, diagnostic parsing, the category
rules, the source scan and the baseline ratchet are checked in seconds. The
client source listing still goes through real ``cmake -P`` over the real lists.
"""

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_x64_compile_census as census  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "build-kisakcod-linux.yaml"


def run_main(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = census.main(argv)
    return status, out.getvalue()


class CommandTests(unittest.TestCase):
    ENTRY = {
        "directory": "/build",
        "command": "/usr/bin/clang++ -DKISAK_MP -I/src -m64 -O2 -o CMakeFiles/KisakCOD-dedi.dir/a.cpp.o -c /src/a.cpp",
        "file": "/src/a.cpp",
        "output": "CMakeFiles/KisakCOD-dedi.dir/a.cpp.o",
    }

    def test_the_command_becomes_a_front_end_pass_with_hazard_warnings(self):
        argv = census.syntax_command(self.ENTRY)
        self.assertEqual(argv[:5], ["/usr/bin/clang++", "-DKISAK_MP", "-I/src", "-m64", "-O2"])
        self.assertIn("/src/a.cpp", argv)
        self.assertNotIn("-c", argv)
        self.assertNotIn("-o", argv)
        self.assertFalse(any(arg.endswith(".o") for arg in argv))
        for flag in ("-fsyntax-only", "-ferror-limit=0", "-Wpointer-to-int-cast", "-Wshorten-64-to-32"):
            self.assertIn(flag, argv)

    def test_the_arguments_form_is_accepted(self):
        entry = dict(self.ENTRY)
        del entry["command"]
        entry["arguments"] = ["clang", "-m64", "-o", "x.o", "-c", "/src/a.c"]
        self.assertEqual(census.syntax_command(entry)[:3], ["clang", "-m64", "/src/a.c"])

    def test_an_i386_command_is_refused(self):
        entry = dict(self.ENTRY, command="clang++ -m32 -c /src/a.cpp")
        with self.assertRaises(census.CensusError):
            census.syntax_command(entry)

    def test_only_the_dedicated_target_is_taken(self):
        other = dict(self.ENTRY, output="CMakeFiles/TracyClient.dir/x.cpp.o", file="/x.cpp",
                     command="clang++ -c /x.cpp")
        self.assertEqual(census.dedi_entries([self.ENTRY, other]), [self.ENTRY])
        with self.assertRaises(census.CensusError):
            census.dedi_entries([other])

    def test_client_units_use_the_client_census_line_at_64_bits(self):
        argv = census.client_command(ROOT / "src" / "client" / "cl_main.cpp", Path("/gen"))
        self.assertIn("-m64", argv)
        self.assertNotIn("-m32", argv)
        self.assertIn("-DKISAK_OPENAL", argv)
        self.assertNotIn("-DDEDICATED", argv)
        self.assertEqual(argv.count("-fsyntax-only"), 1)
        self.assertLess(argv.index("/gen"), argv.index(str(ROOT / "src")))


class DiagnosticTests(unittest.TestCase):
    def test_paths_are_normalized_relative_to_the_repository(self):
        text = "\n".join([
            f"{ROOT}/src/script/../qcommon/qcommon.h:1608:13: error: definition of builtin function '_BitScanReverse'",
            "src/a.cpp:4:2: warning: cast to smaller integer type 'int' from 'char *' [-Wpointer-to-int-cast]",
            "/usr/include/stdio.h:1:1: fatal error: something",
            "In file included from src/a.cpp:1:",
            "1 error generated.",
        ])
        found = census.parse_diagnostics(text, ROOT)
        self.assertEqual(found[0][:3], ("src/qcommon/qcommon.h", 1608, "error"))
        self.assertEqual(found[1], ("src/a.cpp", 4, "warning", "cast to smaller integer type 'int' from 'char *'",
                                    "-Wpointer-to-int-cast"))
        self.assertEqual(found[2][:3], ("/usr/include/stdio.h", 1, "error"))
        self.assertEqual(len(found), 3)

    def test_relative_paths_resolve_against_the_unit_directory(self):
        found = census.parse_diagnostics("../src/a.h:1:1: error: x", ROOT / "build")
        self.assertEqual(found[0][0], "src/a.h")

    def test_categories(self):
        cases = [
            (("src/linux/compat.h", "error", "static assertion failed due to requirement 'sizeof(void *) == 4'", None),
             "linux-shim-abi"),
            (("src/bgame/bg_local.h", "error", "static assertion failed due to requirement 'sizeof(pml_t) == 128'", None),
             "layout-assert"),
            (("src/x.h", "error", "static assertion failed due to requirement "
              "'__builtin_offsetof(A, b) == 8'", None), "layout-assert"),
            (("src/ui/ui_shared.h", "error", "requested alignment is less than minimum alignment of 16 for type 'x'", None),
             "alignment"),
            (("src/q.cpp", "error", "cannot initialize a parameter of type '__va_list_tag *' with an lvalue", None),
             "compiler-types"),
            (("src/q.h", "error", "definition of builtin function '_BitScanReverse'", None), "compiler-types"),
            (("src/qcommon/threads_interlock.h", "error", "static assertion failed due to requirement "
              "'sizeof(long) == 4': InterlockedIncrement needs a 4-byte integer type", None), "compiler-types"),
            (("src/q.cpp", "error", "invalid operand for instruction", None), "inline-asm"),
            (("src/q.cpp", "error", "no matching function for call to 'R_SyncGpu'", None), "other-error"),
            (("src/q.cpp", "warning", "cast", "-Wint-to-pointer-cast"), "pointer-truncation"),
            (("src/q.cpp", "warning", "loses precision", "-Wshorten-64-to-32"), "narrowing-64-to-32"),
            (("src/q.cpp", "warning", "unused", "-Wunused-value"), None),
        ]
        for args, expected in cases:
            with self.subTest(args=args):
                self.assertEqual(census.classify(*args), expected)

    def test_every_category_names_its_upstream_reference(self):
        for key, (title, upstream, meaning) in census.CATEGORIES.items():
            with self.subTest(key=key):
                self.assertTrue(title and upstream and meaning)
        self.assertIn("upstream 74", census.CATEGORIES["layout-assert"][1])
        self.assertIn("upstream 28", census.CATEGORIES["compiler-types"][1])
        self.assertIn("upstream 76", census.SCAN_PATTERNS["miles"][1])
        self.assertIn("upstream 75", census.SCAN_PATTERNS["bink"][1])
        self.assertEqual(set(census.WARNING_CATEGORIES.values()) | set(census.ERROR_CATEGORIES),
                         set(census.CATEGORIES))


class ScanTests(unittest.TestCase):
    def test_comments_and_if0_regions_are_not_live(self):
        text = "\n".join([
            "int a; // __asm { nop }",
            "/* __asm { nop }",
            "   __asm { nop } */",
            "#if 0",
            "__asm { lvx128 v0, r0, r11 }",
            "#ifdef X",
            "__asm { nested }",
            "#endif",
            "#else",
            "__asm fld x",
            "#endif",
            'const char *s = "// not a comment"; __asm fistp i',
        ])
        lines = census.live_code(text)
        self.assertEqual(len(lines), 12)
        live = [number for number, line in enumerate(lines, 1) if "__asm" in line]
        self.assertEqual(live, [10, 12])
        self.assertIn('"// not a comment"', lines[11])

    def test_patterns(self):
        pattern = census.SCAN_PATTERNS["literal-size-copy"][2]
        self.assertTrue(pattern.search("Com_Memcpy((char *)&revBp, (char *)bp, 64);"))
        self.assertTrue(pattern.search("memset(x, 0, 0x40u);"))
        self.assertFalse(pattern.search("memset(x, 0, sizeof(*x));"))
        self.assertTrue(census.SCAN_PATTERNS["fastfile-load-stream"][2].search("Load_Stream(1, p, 4);"))
        self.assertTrue(census.SCAN_PATTERNS["miles"][2].search("AIL_set_sample_volume(s, 1);"))
        self.assertTrue(census.SCAN_PATTERNS["bink"][2].search("BinkOpen(name, 0);"))
        self.assertTrue(census.SCAN_PATTERNS["layout-assert"][2].search("static_assert(sizeof(X) == 4);"))
        self.assertFalse(census.SCAN_PATTERNS["layout-assert"][2].search("static_assert(N > 4);"))

    def test_scan_counts_live_code_and_skips_the_editor(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src" / "game").mkdir(parents=True)
            (root / "src" / "radiant").mkdir(parents=True)
            (root / "src" / "game" / "a.cpp").write_text(
                "#if 0\n__asm { x }\n#endif\n__asm fld x\nstatic_assert(sizeof(int) == 4);\n", encoding="utf-8")
            (root / "src" / "radiant" / "b.cpp").write_text("__asm fld x\n", encoding="utf-8")
            counts = census.scan_sources(root)
        self.assertEqual(counts["inline-asm"], {"src/game/a.cpp": 1})
        self.assertEqual(counts["layout-assert"], {"src/game/a.cpp": 1})


class BaselineTests(unittest.TestCase):
    def write(self, text):
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write(text)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        return Path(handle.name)

    def test_parse_and_refuse_bad_lines(self):
        path = self.write("# c\n\ndedi src/a.cpp # trailing\nclient src/b.cpp\n")
        self.assertEqual(census.load_baseline(path), {("dedi", "src/a.cpp"), ("client", "src/b.cpp")})
        for bad in ("src/a.cpp\n", "server src/a.cpp\n", "dedi src/a.cpp\ndedi src/a.cpp\n"):
            with self.subTest(bad=bad), self.assertRaises(census.CensusError):
                census.load_baseline(self.write(bad))

    def test_compare(self):
        results = {("dedi", "src/ok.cpp"): True, ("dedi", "src/broke.cpp"): False,
                   ("dedi", "src/new.cpp"): True, ("client", "src/x.cpp"): False}
        baseline = {("dedi", "src/ok.cpp"), ("dedi", "src/broke.cpp"), ("dedi", "src/gone.cpp"),
                    ("client", "src/only-client.cpp")}
        regressions, stale, progress = census.compare_baseline(results, baseline, ("dedi",))
        self.assertEqual(regressions, [("dedi", "src/broke.cpp")])
        self.assertEqual(stale, [("dedi", "src/gone.cpp")])  # client entries are not judged by a dedi-only run
        self.assertEqual(progress, [("dedi", "src/new.cpp")])

    def test_write_round_trips(self):
        path = self.write("")
        census.write_baseline(path, {("dedi", "src/b.cpp"): True, ("dedi", "src/a.cpp"): True,
                                     ("client", "src/c.cpp"): False})
        self.assertEqual(census.load_baseline(path), {("dedi", "src/a.cpp"), ("dedi", "src/b.cpp")})
        self.assertTrue(path.read_text(encoding="utf-8").startswith("# Sources that compile"))

    def test_the_checked_in_baseline_names_real_sources(self):
        entries = census.load_baseline(census.BASELINE)
        self.assertGreater(len(entries), 0)
        for name, path in entries:
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file(), path)


class ReportTests(unittest.TestCase):
    def test_report_and_markdown(self):
        results = {("dedi", "src/a.cpp"): True, ("dedi", "src/b.cpp"): False}
        findings = {"layout-assert": {("src/b.h", 3, "static assertion failed"): {("dedi", "src/b.cpp")}}}
        scan = {"inline-asm": {"src/q.h": 2}}
        report = census.build_report(results, findings, scan, ["src/r_init.cpp"])
        self.assertEqual(report["sets"], {"dedi": {"sources": 2, "compiled": 1}})
        self.assertEqual(report["categories"]["layout-assert"]["sites"], 1)
        self.assertEqual(report["categories"]["alignment"]["sites"], 0)
        self.assertEqual(report["scan"]["inline-asm"]["total"], 2)
        text = census.render_markdown(report)
        self.assertIn("upstream 74", text)
        self.assertNotRegex(text, r"#\d")
        self.assertIn("`src/b.h:3` (1 units)", text)
        self.assertIn("1 client sources were skipped", text)
        json.dumps(report)


@unittest.skipUnless(shutil.which("cmake"), "cmake is required to list the client sources")
class FakeCompilerTests(unittest.TestCase):
    """End-to-end runs of main() without a real compiler or configure."""

    def setUp(self):
        self._dir = tempfile.TemporaryDirectory(prefix="kisak-x64-census-test-")
        self.addCleanup(self._dir.cleanup)
        self.dir = Path(self._dir.name)

    def fake_compiler(self, failing=(), hanging=()):
        lines = ["#!/bin/sh", "for arg; do case \"$arg\" in *.c|*.cpp) unit=$arg;; esac; done"]
        for path in failing:
            lines.append(f'case "$unit" in *{path}) echo "$unit:3:1: error: static assertion failed due to '
                         f'requirement \'sizeof(X) == 4\'"; '
                         f'echo "$unit:9:1: warning: cast to smaller integer type \'int\' from \'char *\' '
                         f'[-Wpointer-to-int-cast]"; exit 1;; esac')
        for path in hanging:
            lines.append(f'case "$unit" in *{path}) sleep 30;; esac')
        lines.append("exit 0")
        script = self.dir / "fake-cxx"
        script.write_text("\n".join(lines) + "\n", encoding="utf-8")
        script.chmod(script.stat().st_mode | stat.S_IXUSR)
        return script

    def build_dir(self, cxx, sources):
        build = self.dir / "build"
        build.mkdir(exist_ok=True)
        entries = [{"directory": str(build), "file": str(ROOT / source),
                    "output": f"scripts/platform/linux/CMakeFiles/KisakCOD-dedi.dir/{source}.o",
                    "command": f"{cxx} -m64 -o x.o -c {ROOT / source}"} for source in sources]
        (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
        return build

    def baseline(self, entries):
        path = self.dir / "baseline.txt"
        path.write_text("".join(f"{name} {source}\n" for name, source in entries), encoding="utf-8")
        return path

    def test_dedi_regression_progress_and_report(self):
        cxx = self.fake_compiler(failing=["src/linux/net.cpp"])
        build = self.build_dir(cxx, ["src/linux/net.cpp", "src/linux/sys_posix.cpp", "deps/zlib/crc32.c"])
        report_dir = self.dir / "report"
        status, output = run_main(["--cxx", str(cxx), "--set", "dedi", "--build-dir", str(build),
                                   "--report-dir", str(report_dir), "--baseline",
                                   str(self.baseline([("dedi", "src/linux/net.cpp"),
                                                      ("dedi", "src/linux/sys_posix.cpp")]))])
        self.assertEqual(status, 1, output)
        self.assertIn("FAIL: regression: dedi src/linux/net.cpp", output)
        self.assertIn("progress: dedi deps/zlib/crc32.c", output)
        self.assertNotIn("PASS", output)
        report = json.loads((report_dir / "x64-census.json").read_text(encoding="utf-8"))
        self.assertEqual(report["sets"]["dedi"], {"sources": 3, "compiled": 2})
        # src/linux is the fork's own shim layer, whatever the message says.
        self.assertEqual(report["categories"]["linux-shim-abi"]["sites"], 1)
        self.assertEqual(report["categories"]["pointer-truncation"]["sites"], 1)
        self.assertIn("upstream 74", (report_dir / "x64-census.md").read_text(encoding="utf-8"))
        log = report_dir / "logs" / "dedi" / "src_linux_net.cpp.log"
        self.assertIn("-fsyntax-only", log.read_text(encoding="utf-8"))

    def test_matching_baseline_passes_and_update_rewrites_it(self):
        cxx = self.fake_compiler(failing=["src/linux/net.cpp"])
        build = self.build_dir(cxx, ["src/linux/net.cpp", "src/linux/sys_posix.cpp"])
        baseline = self.baseline([("dedi", "src/linux/sys_posix.cpp")])
        status, output = run_main(["--cxx", str(cxx), "--set", "dedi", "--build-dir", str(build),
                                   "--baseline", str(baseline)])
        self.assertEqual(status, 0, output)
        self.assertIn("PASS", output)

    def test_client_set_skips_i386_blocked_sources(self):
        cxx = self.fake_compiler(failing=["src/client/cl_console.cpp"])
        report_dir = self.dir / "report"
        status, output = run_main(["--cxx", str(cxx), "--set", "client", "--jobs", "8",
                                   "--report-dir", str(report_dir),
                                   "--baseline", str(self.baseline([("client", "src/client/cl_console.cpp")]))])
        self.assertEqual(status, 1, output)
        self.assertIn("regression: client src/client/cl_console.cpp", output)
        report = json.loads((report_dir / "x64-census.json").read_text(encoding="utf-8"))
        self.assertIn("src/gfx_d3d/r_init.cpp", report["skipped"])
        self.assertNotIn("src/gfx_d3d/r_init.cpp", output)
        self.assertEqual(report["categories"]["layout-assert"]["sites"], 1)
        log = (report_dir / "logs" / "client" / "src_client_cl_console.cpp.log").read_text(encoding="utf-8")
        self.assertIn("-m64", log)
        self.assertNotIn("-m32", log)

    def test_a_hung_compile_fails(self):
        cxx = self.fake_compiler(hanging=["src/linux/net.cpp"])
        build = self.build_dir(cxx, ["src/linux/net.cpp"])
        status, output = run_main(["--cxx", str(cxx), "--set", "dedi", "--build-dir", str(build),
                                   "--timeout", "2", "--baseline", str(self.baseline([]))])
        self.assertEqual(status, 1)
        self.assertIn("timeout: dedi src/linux/net.cpp", output)

    def test_a_missing_compiler_fails(self):
        status, output = run_main(["--cxx", str(self.dir / "nope"), "--set", "dedi"])
        self.assertEqual(status, 1)
        self.assertIn("not found", output)


class CMakeKnobTests(unittest.TestCase):
    """i386 stays the default; x86_64 is opt-in (dedicated server and census)."""

    def test_i386_is_the_default_and_x86_64_is_opt_in(self):
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('set(KISAK_LINUX_ARCH "i386" CACHE STRING', text)
        self.assertIn("set(KISAK_LINUX_ARCH_FLAG -m32)", text)
        self.assertIn("set(KISAK_LINUX_ARCH_FLAG -m64)", text)
        self.assertLess(text.index("KISAK_LINUX_ARCH_FLAG"), text.index("project(KisakCOD)"))
        script = (ROOT / "build-linux.sh").read_text(encoding="utf-8")
        self.assertNotIn("KISAK_LINUX_ARCH", script)

    def test_the_platform_keeps_the_32_bit_check_and_fences_x86_64(self):
        text = (ROOT / "scripts" / "platform" / "linux" / "platform.cmake").read_text(encoding="utf-8")
        self.assertIn('elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)', text)
        for option in ("KISAK_BUILD_LINUX_CLIENT_CORE", "KISAK_BUILD_LINUX_WINDOW_HOST",
                       "KISAK_BUILD_LINUX_NATIVE_D3D9", "KISAK_BUILD_DEVICE_PROBE",
                       "KISAK_BUILD_WORLD_DRAW_PROBE"):
            with self.subTest(option=option):
                self.assertIn(option, text)


class WorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(WORKFLOW.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        cls.job = cls.workflow["jobs"]["x64_census"]
        cls.commands = "\n".join(step.get("run", "") for step in cls.job["steps"])

    def test_the_job_runs_the_contract_tests_then_the_census(self):
        names = [step.get("name") for step in self.job["steps"]]
        self.assertLess(names.index("Check x64 census runner"), names.index("Run x86_64 compile census"))
        self.assertIn("python3 scripts/tests/test_x64_compile_census.py", self.commands)
        self.assertIn("scripts/tests/run_x64_compile_census.py", self.commands)
        self.assertIn("--report-dir", self.commands)

    def test_every_command_block_propagates_failure(self):
        for step in self.job["steps"]:
            if "run" in step:
                with self.subTest(step=step.get("name")):
                    self.assertTrue(step["run"].lstrip().startswith("set -euo pipefail"))

    def test_the_job_needs_no_i386_toolchain_graphics_or_assets(self):
        for forbidden in ("multilib", "-m32", "dxvk", "sdl", "vulkan", "xvfb", "steam", "build-linux.sh",
                          "cmake --build"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.commands.lower())

    def test_report_is_kept_on_failure(self):
        upload = next(step for step in self.job["steps"]
                      if step.get("uses", "").startswith("actions/upload-artifact@"))
        self.assertEqual(upload["if"], "always()")


class X64DedicatedWorkflowTests(unittest.TestCase):
    """The x86_64 dedicated server job builds each config and boots it."""

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(WORKFLOW.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        cls.job = cls.workflow["jobs"]["x64_dedicated"]
        cls.commands = "\n".join(step.get("run", "") for step in cls.job["steps"])

    def test_both_configs_build_and_boot_as_x86_64(self):
        self.assertEqual(self.job["strategy"]["matrix"]["config"], ["Debug", "RelWithDebInfo"])
        self.assertIn("-DKISAK_LINUX_ARCH=x86_64", self.commands)
        self.assertIn("--target KisakCOD-dedi", self.commands)
        self.assertIn('scripts/tests/test_linux_bootstrap.py --arch x86_64 "$BINARY"', self.commands)

    def test_layout_tables_and_conversion_are_checked_before_the_build(self):
        names = [step.get("name") for step in self.job["steps"]]
        self.assertIn('scripts/tools/gen_db_disk_layout.py --build-dir "$BUILD_DIR" --check', self.commands)
        self.assertIn("scripts/tests/test_db_record_conversion.py", self.commands)
        self.assertLess(names.index("Check fastfile record layout tables"), names.index("Build x86_64 KisakCOD-dedi"))
        self.assertLess(names.index("Check 64-bit fastfile record conversion"), names.index("Build x86_64 KisakCOD-dedi"))

    def test_every_command_block_propagates_failure(self):
        for step in self.job["steps"]:
            if "run" in step:
                with self.subTest(step=step.get("name")):
                    self.assertTrue(step["run"].lstrip().startswith("set -euo pipefail"))

    def test_the_job_needs_no_i386_toolchain_graphics_or_assets(self):
        # Only the layout check reads i386 headers; no other step may use them.
        commands = "\n".join(step.get("run", "") for step in self.job["steps"]
                              if step.get("name") != "Install i386 headers for the layout check")
        for forbidden in ("multilib", "-m32", "dxvk", "sdl", "vulkan", "xvfb", "steam", "build-linux.sh"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, commands.lower())

    def test_i386_headers_are_installed_before_the_layout_check(self):
        names = [step.get("name") for step in self.job["steps"]]
        headers = names.index("Install i386 headers for the layout check")
        self.assertLess(headers, names.index("Check fastfile record layout tables"))
        step = self.job["steps"][headers]
        self.assertIn("g++-multilib", step["run"])


if __name__ == "__main__":
    unittest.main()
