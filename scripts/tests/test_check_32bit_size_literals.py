#!/usr/bin/env python3
"""Self-test for scripts/tools/check_32bit_size_literals.py.

Scans a small generated C++ file with the tool's own scan() and checks which
lines it reports: literals equal to a pointer-holding record's i386 size, a 4
that multiplies a count of 4-byte (i386) records, and pointer arrays sized in
4-byte entries. Array bounds, indexes, flags and n * sizeof(T) counts must stay
quiet. Also checks that a source both games compile is listed once for each. Needs the libclang Python bindings (python3-clang-18), like the tool;
run it with the system Python.
"""

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts" / "tools" / "check_32bit_size_literals.py"

SOURCE = """\
typedef unsigned long size_t;
void *Alloc(size_t size);
void *memset(void *dst, int value, size_t size);
struct Holder { void *p; };
union Small { float f; void *p; };
struct Big { void *p[4]; };
void cases(int n)
{
    Holder *a = (Holder *)Alloc(4 * n); // EXPECT
    memset(a, 0, 4 * n); // EXPECT
    Small *b = (Small *)Alloc(sizeof(Small) * n);
    Small *c = (Small *)Alloc(sizeof(Small) * 4);
    Holder h[4];
    a[4].p = b;
    int flags = n & 4;
    Big *d = (Big *)Alloc(16); // EXPECT
    void **e = (void **)Alloc(4 * n); // EXPECT
    (void)c; (void)h; (void)flags; (void)d; (void)e;
}
"""


def load_tool():
    spec = importlib.util.spec_from_file_location("check_32bit_size_literals", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SizeLiteralTests(unittest.TestCase):
    def test_reports(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cases.cpp"
            path.write_text(SOURCE, encoding="utf-8")
            _, hits, errors = tool.scan((str(path), ["-x", "c++", "-std=c++17"], tmp, 8))
        self.assertEqual(errors, [])
        reported = {int(text.split(":")[1]) for _, text in hits}
        expected = {i + 1 for i, line in enumerate(SOURCE.splitlines()) if "// EXPECT" in line}
        self.assertEqual(reported, expected, "\n".join(text for _, text in hits))

    def test_a_shared_source_is_read_once_for_each_game(self):
        tool = load_tool()
        shared = ROOT / "src" / "universal" / "q_shared.cpp"
        command = lambda *defines: {"directory": str(ROOT), "file": str(shared),
                                    "command": " ".join(["clang++", *defines, "-c", str(shared)])}
        entries = [command("-DKISAK_MP"), command("-DKISAK_SP", "-DKISAK_OPENAL"), command("-DKISAK_MP", "-DOTHER"),
                   command("-DKISAK_SP"),
                   {"directory": "/elsewhere", "file": "/elsewhere/not_ours.cpp", "command": "clang++ -c not_ours.cpp"}]
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
            units = list(tool.translation_units(Path(tmp)))
        self.assertEqual([path for path, _, _ in units], [str(shared), str(shared)])
        self.assertEqual([tool.singleplayer_unit(args) for _, args, _ in units], [False, True])
        self.assertEqual(units[1][1], ["-DKISAK_SP", "-DKISAK_OPENAL"])


if __name__ == "__main__":
    unittest.main()
