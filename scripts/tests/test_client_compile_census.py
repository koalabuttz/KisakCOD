#!/usr/bin/env python3
"""Asset-free tests for run_client_compile_census.py and its CI job.

The census itself compiles 400+ sources; these tests instead drive it with a
fake compiler (a shell script that fails for chosen paths) so the ratchet rules,
group coverage, list parsing and timeout handling are checked in seconds. The
source listing still goes through real ``cmake -P`` over the real CMake lists.
"""

import contextlib
import io
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import textwrap
import unittest

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_client_compile_census as census  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "build-kisakcod-linux.yaml"


def run_main(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = census.main(argv)
    return status, out.getvalue()


class ParsingTests(unittest.TestCase):
    def test_target_groups_reads_the_add_executable_block(self):
        text = "add_executable(${PROJECT_NAME}\n    ${SRCROOT}\n    ${GFX_D3D} \n)\nset(X ${NOT_A_GROUP})\n"
        self.assertEqual(census.target_groups(text), ["SRCROOT", "GFX_D3D"])

    def test_target_groups_refuses_a_target_without_the_block(self):
        with self.assertRaises(census.CensusError):
            census.target_groups("project(x)\n")

    def test_the_real_mp_target_is_fully_covered(self):
        groups = census.target_groups(census.MP_TARGET.read_text(encoding="utf-8"))
        self.assertEqual(census.check_group_coverage(groups), [])
        self.assertIn("GFX_D3D", groups)
        self.assertIn("WIN32_SRC", census.EXCLUDED_GROUPS)

    def test_group_drift_in_either_direction_is_reported(self):
        groups = list(census.CENSUS_GROUPS) + list(census.EXCLUDED_GROUPS)
        gained = census.check_group_coverage(groups + ["NEW_GROUP"])
        self.assertEqual(len(gained), 1)
        self.assertIn("NEW_GROUP", gained[0])
        lost = census.check_group_coverage([group for group in groups if group != "XANIM"])
        self.assertEqual(len(lost), 1)
        self.assertIn("XANIM", lost[0])

    def test_census_and_excluded_groups_do_not_overlap(self):
        self.assertFalse(set(census.CENSUS_GROUPS) & set(census.EXCLUDED_GROUPS))

    def test_listing_keeps_cpp_only_and_drops_duplicates(self):
        text = "\n".join([
            f"GFX_D3D|{ROOT}/src/gfx_d3d/r_bsp.cpp",
            f"GFX_D3D|{ROOT}/src/gfx_d3d/r_bsp.h",
            f"COMMON|{ROOT}/src/gfx_d3d/r_bsp.cpp",
            "noise without a separator",
        ])
        parsed = census.parse_listing(text)
        self.assertEqual([(group, census.relative(path)) for group, path in parsed],
                         [("GFX_D3D", "src/gfx_d3d/r_bsp.cpp")])


class BlockedListTests(unittest.TestCase):
    def write(self, text):
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write(text)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        return Path(handle.name)

    def test_comments_blank_lines_and_reasons(self):
        path = self.write("# header\n\nsrc/a.cpp: needs D3D9 # trailing\nsrc/b.cpp: curl\n")
        self.assertEqual(census.load_blocked(path), {"src/a.cpp": "needs D3D9", "src/b.cpp": "curl"})

    def test_an_entry_without_a_reason_is_refused(self):
        with self.assertRaises(census.CensusError):
            census.load_blocked(self.write("src/a.cpp:\n"))
        with self.assertRaises(census.CensusError):
            census.load_blocked(self.write("src/a.cpp\n"))

    def test_a_duplicate_entry_is_refused(self):
        with self.assertRaises(census.CensusError):
            census.load_blocked(self.write("src/a.cpp: x\nsrc/a.cpp: y\n"))

    def test_compare_finds_regressions_and_stale_entries(self):
        results = {"src/ok.cpp": True, "src/new_failure.cpp": False, "src/listed.cpp": False,
                   "src/fixed.cpp": True}
        blocked = {"src/listed.cpp": "r", "src/fixed.cpp": "r", "src/gone.cpp": "r"}
        regressions, stale = census.compare(results, blocked)
        self.assertEqual(regressions, ["src/new_failure.cpp"])
        self.assertEqual(stale, ["src/fixed.cpp", "src/gone.cpp"])

    def test_the_checked_in_list_parses_and_names_real_sources(self):
        blocked = census.load_blocked(census.BLOCKED_LIST)
        self.assertGreater(len(blocked), 0)
        for path in blocked:
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file(), path)
                self.assertTrue(path.endswith(".cpp"))


@unittest.skipUnless(shutil.which("cmake"), "cmake is required to list the client sources")
class FakeCompilerTests(unittest.TestCase):
    """End-to-end runs of main() with a compiler that fails only where told to."""

    def setUp(self):
        self._dir = tempfile.TemporaryDirectory(prefix="kisak-census-test-")
        self.addCleanup(self._dir.cleanup)
        self.dir = Path(self._dir.name)

    def fake_compiler(self, failing=(), hanging=()):
        lines = ["#!/bin/sh", 'for last; do :; done']
        for path in failing:
            lines.append(f'case "$last" in *{path}) echo "error: fake failure" >&2; exit 1;; esac')
        for path in hanging:
            lines.append(f'case "$last" in *{path}) sleep 30;; esac')
        lines.append("exit 0")
        script = self.dir / "fake-cxx"
        script.write_text("\n".join(lines) + "\n", encoding="utf-8")
        script.chmod(script.stat().st_mode | stat.S_IXUSR)
        return script

    def blocked(self, entries):
        path = self.dir / "blocked.txt"
        path.write_text("".join(f"{entry}: reason\n" for entry in entries), encoding="utf-8")
        return path

    def test_matching_failures_pass(self):
        cxx = self.fake_compiler(failing=["src/gfx_d3d/r_init.cpp"])
        status, output = run_main(["--cxx", str(cxx), "--jobs", "8",
                                   "--blocked", str(self.blocked(["src/gfx_d3d/r_init.cpp"]))])
        self.assertEqual(status, 0, output)
        self.assertIn("PASS: client compile census", output)

    def test_an_unlisted_failure_is_a_regression(self):
        cxx = self.fake_compiler(failing=["src/gfx_d3d/r_dpvs.cpp"])
        log_dir = self.dir / "logs"
        status, output = run_main(["--cxx", str(cxx), "--jobs", "8", "--log-dir", str(log_dir),
                                   "--blocked", str(self.blocked([]))])
        self.assertEqual(status, 1)
        self.assertIn("regression: src/gfx_d3d/r_dpvs.cpp", output)
        self.assertNotIn("PASS", output)
        log = log_dir / "src_gfx_d3d_r_dpvs.cpp.log"
        self.assertTrue(log.is_file())
        self.assertIn("fake failure", log.read_text(encoding="utf-8"))
        self.assertIn("-m32", log.read_text(encoding="utf-8"))

    def test_a_listed_source_that_compiles_is_stale(self):
        cxx = self.fake_compiler()
        status, output = run_main(["--cxx", str(cxx), "--jobs", "8",
                                   "--blocked", str(self.blocked(["src/gfx_d3d/r_init.cpp"]))])
        self.assertEqual(status, 1)
        self.assertIn("stale: src/gfx_d3d/r_init.cpp now compiles", output)

    def test_a_listed_path_outside_the_target_is_stale(self):
        cxx = self.fake_compiler()
        status, output = run_main(["--cxx", str(cxx), "--jobs", "8",
                                   "--blocked", str(self.blocked(["src/linux/main.cpp"]))])
        self.assertEqual(status, 1)
        self.assertIn("src/linux/main.cpp is not in the client source lists", output)

    def test_a_hung_compile_is_a_failure_even_when_listed(self):
        cxx = self.fake_compiler(hanging=["src/gfx_d3d/r_bsp.cpp"])
        status, output = run_main(["--cxx", str(cxx), "--jobs", "8", "--timeout", "2",
                                   "--blocked", str(self.blocked(["src/gfx_d3d/r_bsp.cpp"]))])
        self.assertEqual(status, 1)
        self.assertIn("timeout: src/gfx_d3d/r_bsp.cpp", output)

    def test_a_missing_compiler_fails_instead_of_passing(self):
        status, output = run_main(["--cxx", str(self.dir / "no-such-cxx"),
                                   "--blocked", str(self.blocked([]))])
        self.assertEqual(status, 1)
        self.assertIn("not found", output)

    def test_a_malformed_list_fails_setup(self):
        path = self.dir / "bad.txt"
        path.write_text("src/gfx_d3d/r_init.cpp\n", encoding="utf-8")
        status, output = run_main(["--cxx", str(self.fake_compiler()), "--blocked", str(path)])
        self.assertEqual(status, 1)
        self.assertIn("census setup", output)

    def test_the_compile_line_is_a_32_bit_client_front_end_pass(self):
        self.assertIn("-m32", census.CLIENT_FLAGS)
        self.assertIn("-fsyntax-only", census.CLIENT_FLAGS)
        for define in ("-DKISAK_MP", "-DKISAK_OPENAL", "-DRELEASE_ASSERTS"):
            self.assertIn(define, census.CLIENT_DEFINES)
        self.assertNotIn("-DDEDICATED", census.CLIENT_DEFINES)
        self.assertNotIn("-DKISAK_DEDICATED", census.CLIENT_DEFINES)


class WorkflowTests(unittest.TestCase):
    """The hosted job: toolchain plus OpenAL headers, no graphics, no assets."""

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(WORKFLOW.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        cls.job = cls.workflow["jobs"]["client_census"]
        cls.commands = "\n".join(step.get("run", "") for step in cls.job["steps"])

    def test_the_job_runs_the_contract_tests_then_the_census(self):
        names = [step.get("name") for step in self.job["steps"]]
        self.assertLess(names.index("Check client census runner"), names.index("Run client compile census"))
        self.assertIn("python3 scripts/tests/test_client_compile_census.py", self.commands)
        self.assertIn("scripts/tests/run_client_compile_census.py", self.commands)
        self.assertIn("--cxx clang++", self.commands)

    def test_every_command_block_propagates_failure(self):
        for step in self.job["steps"]:
            if "run" in step:
                with self.subTest(step=step.get("name")):
                    self.assertTrue(step["run"].lstrip().startswith("set -euo pipefail"))

    def test_the_job_needs_no_graphics_stack_assets_or_engine_build(self):
        for forbidden in ("dxvk", "sdl", "vulkan", "xvfb", "steam", "cmake -s", "cmake --build",
                          "build-linux.sh", "ninja"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.commands.lower())
        install = next(step for step in self.job["steps"] if step.get("name") == "Install client census tools")
        self.assertIn("libopenal-dev", install["run"])
        self.assertIn("g++-multilib", install["run"])

    def test_logs_are_kept_on_failure(self):
        upload = next(step for step in self.job["steps"]
                      if step.get("uses", "").startswith("actions/upload-artifact@"))
        self.assertEqual(upload["if"], "always()")


if __name__ == "__main__":
    unittest.main()
