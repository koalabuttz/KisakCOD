#!/usr/bin/env python3
"""Self-test for scripts/tools/check_32bit_layout_offsets.py.

Scans a small generated C++ file with the tool's own scan() and checks which
lines it reports: constant byte offsets that land on another field on x86_64,
pointer-sized values read at a byte offset, unions copied through a 4-byte
member, member arrays indexed past their end into a neighbour whose x86_64
position differs, and `K * n` strides equal to a changed i386 size. Offsets
into pointer-free structs, sizeof-based offsets and offsets before the first
pointer must stay quiet. Needs the libclang Python bindings (python3-clang-18),
like the tool; run it with the system Python.
"""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts" / "tools" / "check_32bit_layout_offsets.py"

SOURCE = """\
typedef __UINTPTR_TYPE__ uintptr_t;
union Value { int i; float f; const char *s; };
struct Context { int kind; void *owner; float scale; const float *dir; };
struct Plain { int a; float b[4]; int c; };
struct Entry { void *p; int n; };
struct Table { int count; Entry entries[8]; float tail; };
struct Lods { int count; void *list; float dist[4]; float after; };
struct Elem { int type; int x; int y; };
struct Spill { float dist[4]; void *list; float after; };
float cases(void *raw, Context *ctx, Plain *plain, Table *table, Lods *lods, Spill *spill, Elem *elem, int i, int ofs)
{
    float s = *(float *)((char *)ctx + 8); // EXPECT
    s += *(float *)((char *)plain + 20);
    s += *(const float **)((uintptr_t)raw + 12) != 0; // EXPECT
    *(Value *)((char *)&elem->type + ofs) = Value(); // EXPECT
    *(int *)((char *)&elem->type + ofs) = 1;
    Entry *e = (Entry *)((char *)table->entries + 8 * i); // EXPECT
    Entry *f = (Entry *)((char *)table->entries + sizeof(Entry) * i);
    s += spill->dist[5]; // EXPECT
    s += lods->dist[4];
    s += *((float *)&lods->list + 2); // EXPECT
    s += *(int *)((char *)ctx + 0) + plain->b[3];
    Value a, b;
    a.i = b.i; // EXPECT
    a = b;
    return s + (e != f) + a.f;
}
"""


def load_tool():
    sys.path.insert(0, str(TOOL.parent))
    spec = importlib.util.spec_from_file_location("check_32bit_layout_offsets", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LayoutOffsetTests(unittest.TestCase):
    def test_reports(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cases.cpp"
            path.write_text(SOURCE, encoding="utf-8")
            _, hits, errors = tool.scan((str(path), ["-x", "c++", "-std=c++17"], tmp))
        self.assertEqual(errors, [])
        reported = {int(text.split(":")[1]) for _, text in hits}
        expected = {i + 1 for i, line in enumerate(SOURCE.splitlines()) if "// EXPECT" in line}
        self.assertEqual(reported, expected, "\n".join(text for _, text in hits))


if __name__ == "__main__":
    unittest.main()
