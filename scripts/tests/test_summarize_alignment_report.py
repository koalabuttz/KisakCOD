#!/usr/bin/env python3
"""Self-tests for scripts/tools/summarize_alignment_report.py.

The input below is a real UBSan report from clang -m32 -fsanitize=alignment
(paths shortened): a float read at an odd address, a structure member read
through an odd pointer (reported twice: the member access and the float load),
and an int read at an odd address.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "tools" / "summarize_alignment_report.py"
spec = importlib.util.spec_from_file_location("summarize_alignment_report", TOOL)
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)

REPORT = """\
/w/KisakCOD/src/script/scr_vm.cpp:4:60: runtime error: load of misaligned address 0x56f67f7d for type 'const float', which requires 4 byte alignment
0x56f67f7d: note: pointer points here
 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  00
             ^
    #0 0x5667c220 in rf(char const*) /w/KisakCOD/src/script/scr_vm.cpp:4:60
    #1 0x5667c2f0 in VM_Execute /w/KisakCOD/src/script/scr_vm.cpp:7:24
    #2 0xf7951c74  (/lib32/libc.so.6+0x24c74) (BuildId: a6ac4013957ce900e167743cd60a25daff1344bb)
    #3 0xf7951d37 in __libc_start_main (/lib32/libc.so.6+0x24d37) (BuildId: a6ac4013957ce900e167743cd60a25daff1344bb)

SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /w/KisakCOD/src/script/scr_vm.cpp:4:60
/w/KisakCOD/src/game_mp/g_main_mp.cpp:5:76: runtime error: member access within misaligned address 0x56f67f7e for type 'const gentity_s', which requires 4 byte alignment
0x56f67f7e: note: pointer points here
 00 00 00 00 00 00  00 00 00 00 00 00 00 00  00 00
             ^
    #0 0x5667c268 in rs(char const*) /w/KisakCOD/src/game_mp/g_main_mp.cpp:5:76
    #1 0x5667c301 in G_RunFrame /w/KisakCOD/src/game_mp/g_main_mp.cpp:7:38

SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /w/KisakCOD/src/game_mp/g_main_mp.cpp:5:76
/w/KisakCOD/src/game_mp/g_main_mp.cpp:5:76: runtime error: load of misaligned address 0x56f67f82 for type 'const float', which requires 4 byte alignment
0x56f67f82: note: pointer points here
 00 00  00 00 00 00 00 00 00 00  00 00 00 00 00 00
              ^
    #0 0x5667c281 in rs(char const*) /w/KisakCOD/src/game_mp/g_main_mp.cpp:5:76
    #1 0x5667c301 in G_RunFrame /w/KisakCOD/src/game_mp/g_main_mp.cpp:7:38

SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /w/KisakCOD/src/game_mp/g_main_mp.cpp:5:76
/w/KisakCOD/src/qcommon/msg.cpp:6:58: runtime error: load of misaligned address 0x56f67f7f for type 'const int', which requires 4 byte alignment
0x56f67f7f: note: pointer points here
 00 00 00 00 00  00 00 00 00 00 00 00 00  00 00 00
             ^
    #0 0x5667c2c0 in ri(char const*) /w/KisakCOD/src/qcommon/msg.cpp:6:58
    #1 0x5667c317 in MSG_ReadLong /w/KisakCOD/src/qcommon/msg.cpp:7:59

SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /w/KisakCOD/src/qcommon/msg.cpp:6:58
"""


class ClassifyTests(unittest.TestCase):
    def test_word_aligned_is_always_fine(self):
        for type_name in ("float", "double", "long long", "gentity_s", "int"):
            self.assertEqual(summary.classify(type_name, 0x1000), "ok", type_name)
            self.assertEqual(summary.classify(type_name, 0x1004), "ok", type_name)

    def test_wide_types_fault_off_a_word(self):
        for type_name in ("float", "const float", "double", "long long", "unsigned long long",
                          "uint64_t", "__int64", "float[3]", "vec3_t"):
            for address in (0x1001, 0x1002, 0x1003):
                self.assertEqual(summary.classify(type_name, address), "faults", type_name)

    def test_single_words_and_pointers_are_fine(self):
        for type_name in ("int", "const unsigned int", "short", "unsigned short", "uint32_t",
                          "char", "float *", "const char *", "gentity_s *"):
            self.assertEqual(summary.classify(type_name, 0x1001), "ok", type_name)

    def test_named_types_need_a_look(self):
        for type_name in ("gentity_s", "const VariableValue", "struct S", "union U",
                          "scrVarPub_t", "std::string"):
            self.assertEqual(summary.classify(type_name, 0x1002), "check", type_name)


class ParseTests(unittest.TestCase):
    def setUp(self):
        self.sites = {(site["file"], site["line"]): site
                      for site in summary.parse(REPORT.splitlines(True))}

    def test_one_site_per_source_line(self):
        self.assertEqual(sorted(self.sites), [
            ("src/game_mp/g_main_mp.cpp", 5),
            ("src/qcommon/msg.cpp", 6),
            ("src/script/scr_vm.cpp", 4),
        ])

    def test_classes_and_types(self):
        float_read = self.sites[("src/script/scr_vm.cpp", 4)]
        self.assertEqual(float_read["class"], "faults")
        self.assertEqual(float_read["addresses"], {5})
        # The worst report on a line decides its class.
        member = self.sites[("src/game_mp/g_main_mp.cpp", 5)]
        self.assertEqual(member["class"], "faults")
        self.assertEqual(member["types"], ["const gentity_s", "const float"])
        self.assertEqual(member["count"], 2)
        self.assertEqual(self.sites[("src/qcommon/msg.cpp", 6)]["class"], "ok")

    def test_callers_keep_source_frames_only(self):
        self.assertEqual(self.sites[("src/script/scr_vm.cpp", 4)]["frames"],
                         ["VM_Execute (src/script/scr_vm.cpp:7:24)"])


class MainTests(unittest.TestCase):
    def run_main(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "ubsan.123"
            log.write_text(text)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = summary.main([str(log)])
        return code, out.getvalue()

    def test_faults_exit_one_and_print_tables(self):
        code, text = self.run_main(REPORT)
        self.assertEqual(code, 1)
        self.assertIn("3 source lines: 2 fault on the Vita, 0 to check, 1 fine.", text)
        self.assertIn("| src/script/scr_vm.cpp:4 | load of | `const float` | 5 | 1 |", text)

    def test_clean_log_exits_zero(self):
        code, text = self.run_main("")
        self.assertEqual(code, 0)
        self.assertIn("0 source lines", text)


if __name__ == "__main__":
    unittest.main()
