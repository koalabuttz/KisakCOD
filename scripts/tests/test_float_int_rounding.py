#!/usr/bin/env python3
"""Pin the retail map-centre rounding named for upstream issue 56.

Retail 1.7 rounds the origin codec's map centre by adding 0.5 and
truncating (MSG_RoundMapCenter), checked against the retail executable with
scripts/audit/retail_float_int_scan.py. This test compiles the helper
straight from the header and checks its values, and checks that the origin
encoder and decoder both use it.
"""

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
QCOMMON = ROOT / "src" / "qcommon" / "qcommon.h"
MSG_READ = ROOT / "src" / "qcommon" / "msg_mp.cpp"
MSG_WRITE = ROOT / "src" / "qcommon" / "sv_msg_write_mp.cpp"


def inline_function(path, name):
    text = path.read_text()
    m = re.search(r"inline [^\n]*\b" + name + r"\([^)]*\)\n\{.*?\n\}", text, re.S)
    if not m:
        raise AssertionError(f"{name} not found in {path}")
    return m.group(0)


PROGRAM = r"""
#include <cstdio>
%s
int main()
{
    const float centres[] = { -1.49f, -1.5f, -100.3f, -0.6f, 0.49f, 2.5f, 3.5f, 1234.75f };
    for (float c : centres)
        std::printf("centre %%g %%d\n", c, MSG_RoundMapCenter(c));
    return 0;
}
"""


class HelperValueTest(unittest.TestCase):
    def test_centre_rounding_matches_retail(self):
        cxx = shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            self.skipTest("no C++ compiler")
        source = PROGRAM % inline_function(QCOMMON, "MSG_RoundMapCenter")
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "rounding.cpp"
            exe = pathlib.Path(tmp) / "rounding"
            src.write_text(source)
            subprocess.run([cxx, "-O2", "-std=c++17", str(src), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        lines = out.splitlines()
        centres = dict(line.split()[1:] for line in lines)
        # +0.5 then truncate toward zero: negative centres with a fraction
        # under .5 move up, unlike std::round.
        self.assertEqual(centres, {
            "-1.49": "0", "-1.5": "-1", "-100.3": "-99", "-0.6": "0",
            "0.49": "0", "2.5": "3", "3.5": "4", "1234.75": "1235",
        })


class SourceUseTest(unittest.TestCase):
    def test_codec_uses_one_centre_helper(self):
        reader = MSG_READ.read_text()
        writer = MSG_WRITE.read_text()
        # Client and dedicated branches of the X/Y and Z readers.
        self.assertEqual(reader.count("MSG_RoundMapCenter("), 4)
        # Full and delta paths of the X/Y and Z writers.
        self.assertEqual(writer.count("MSG_RoundMapCenter("), 4)
        for text in (reader, writer):
            self.assertIsNone(re.search(r"mapCenter(?:\(\))?\)?\[\w+\] \+ 0\.5", text))


if __name__ == "__main__":
    unittest.main()
