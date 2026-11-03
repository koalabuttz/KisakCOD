#!/usr/bin/env python3
"""Tests for summarize_msvc_x64.py over hand-written MSBuild output."""

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import summarize_msvc_x64 as summary  # noqa: E402

ROOT = r"D:\a\KisakCOD\KisakCOD"
MP_LOG = "\n".join([
    "  cg_ents.cpp",
    r"D:\a\KisakCOD\KisakCOD\src\qcommon\qcommon.h(1634,5): error C4235: nonstandard extension used: "
    r"'__asm' keyword not supported on this architecture [D:\a\KisakCOD\KisakCOD\build-x64\scripts\mp\KisakCOD-mp.vcxproj]",
    r"D:\a\KisakCOD\KisakCOD\src\qcommon\qcommon.h(1634,5): error C4235: nonstandard extension used: "
    r"'__asm' keyword not supported on this architecture [D:\a\KisakCOD\KisakCOD\build-x64\scripts\mp\KisakCOD-mp.vcxproj]",
    r"D:\a\KisakCOD\KisakCOD\src\bgame\bg_local.h(241,1): error C2338: static_assert failed: "
    r"'sizeof(scr_anim_s) == 4' [D:\a\KisakCOD\KisakCOD\build-x64\scripts\mp\KisakCOD-mp.vcxproj]",
    r"  D:\a\KisakCOD\KisakCOD\src\universal\com_memory.cpp(476,20): warning C4312: 'type cast': conversion "
    r"from 'unsigned int' to 'void *' of greater size [D:\a\KisakCOD\KisakCOD\build-x64\scripts\mp\KisakCOD-mp.vcxproj]",
    r"D:\a\KisakCOD\KisakCOD\src\universal\com_files.cpp(10): warning C4267: '=': conversion from 'size_t' to "
    r"'int', possible loss of data [D:\a\KisakCOD\KisakCOD\build-x64\scripts\mp\KisakCOD-mp.vcxproj]",
    r"D:\a\KisakCOD\KisakCOD\src\x.cpp(3,1): warning C4100: unreferenced [D:\x.vcxproj]",
    r"D:\a\KisakCOD\KisakCOD\src\y.cpp(9,1): error C2664: 'f': cannot convert argument 1 [D:\x.vcxproj]",
    r"C:\Program Files\Windows Kits\10\Include\um\winnt.h(5,1): error C2338: static_assert failed: "
    r"'sizeof(long) == 4' [D:\x.vcxproj]",
])
DEDI_LOG = "\n".join([
    r"D:\a\KisakCOD\KisakCOD\src\bgame\bg_local.h(241,1): error C2338: static_assert failed: "
    r"'sizeof(scr_anim_s) == 4' [D:\a\KisakCOD\KisakCOD\build-x64\scripts\dedi\KisakCOD-dedi.vcxproj]",
])


class ParseTests(unittest.TestCase):
    def test_paths_codes_and_categories(self):
        found = summary.parse_log(MP_LOG, ROOT)
        self.assertEqual(found[0][:4], ("src/qcommon/qcommon.h", 1634, "error", "C4235"))
        self.assertEqual(found[3][:4], ("src/universal/com_memory.cpp", 476, "warning", "C4312"))
        self.assertEqual(found[4][:2], ("src/universal/com_files.cpp", 10))
        categories = [summary.classify(code, kind, message) for _p, _l, kind, code, message in found]
        self.assertEqual(categories, ["inline-asm", "inline-asm", "layout-assert", "pointer-truncation",
                                      "narrowing-64-to-32", None, "other-error", "compiler-types"])

    def test_relative_falls_back_to_src_for_other_checkouts(self):
        self.assertEqual(summary.relative(r"E:\work\kisak\src\a\b.h", ROOT), "src/a/b.h")
        self.assertEqual(summary.relative(r"C:\Program Files\x.h", ROOT), "C:/Program Files/x.h")


class ReportTests(unittest.TestCase):
    def test_sites_are_deduplicated_across_lines_and_targets(self):
        per_target, findings = summary.summarize({"KisakCOD-mp": MP_LOG, "KisakCOD-dedi": DEDI_LOG}, ROOT)
        report = summary.build_report(per_target, findings)
        self.assertEqual(report["categories"]["inline-asm"]["sites"], 1)
        layout = report["categories"]["layout-assert"]
        self.assertEqual(layout["sites"], 1)
        self.assertEqual(layout["examples"][0]["targets"], ["KisakCOD-dedi", "KisakCOD-mp"])
        self.assertEqual(per_target["KisakCOD-dedi"], {"errors": 1, "warnings": 0, "files_with_errors": 1,
                                                       "succeeded": False})
        text = summary.render_markdown(report)
        self.assertIn("upstream issue 74", text)
        self.assertNotIn("#74", text)

    def test_main_writes_reports_and_refuses_an_empty_log(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "mp.log").write_text(MP_LOG, encoding="utf-8")
            (work / "empty.log").write_text("nothing here\n", encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = summary.main(["--root", ROOT, "--report-dir", str(work / "r"),
                                       f"KisakCOD-mp={work / 'mp.log'}"])
            self.assertEqual(status, 0)
            report = json.loads((work / "r" / "msvc-x64-census.json").read_text(encoding="utf-8"))
            self.assertEqual(report["targets"]["KisakCOD-mp"]["errors"], 4)  # the repeated asm line counts once
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(summary.main(["--report-dir", str(work / "r2"),
                                               f"KisakCOD-mp={work / 'empty.log'}"]), 1)
                self.assertEqual(summary.main(["--report-dir", str(work / "r3"),
                                               f"KisakCOD-mp={work / 'missing.log'}"]), 1)


if __name__ == "__main__":
    unittest.main()
