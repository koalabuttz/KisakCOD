#!/usr/bin/env python3
"""Generate src/database/db_disk_layout_gen.h: how each fastfile record type is
laid out on disk (retail 32-bit) and where each piece goes in the native struct.

The fastfile loader reads retail records straight into engine structs. On a
32-bit build that is exact. On a 64-bit build every pointer is 8 bytes, so a
record has to be expanded field by field. This script asks Clang for the layout
of every type the loader streams (the pointee type of each `var*` global in
src/database/db_load.cpp) twice, once as i386 and once as x86_64, and writes a
table of operations per type:

  DB_DISK_COPY  copy `size` bytes from disk offset to native offset
  DB_DISK_PTR   widen a 4-byte retail pointer field into a native pointer

Disk offsets are numbers (the retail layout). Native offsets are written as
`offsetof(Type, member)`, so the compiler computes them for whatever ABI it
builds, and the i386 build checks that each one equals its disk offset.

Types whose two layouts are identical and hold no pointers are "plain" and are
copied as they are. A few unions mix pointers and data; UNION_POLICY says which
member the converter follows (see the comments there).

Needs the libclang Python bindings (Debian/Ubuntu: python3-clang-18, run with
/usr/bin/python3) and a configured i386 Linux build directory for the exact
compile flags (bash build-linux.sh build-linux, or any KisakCOD-dedi configure).

Usage:
    /usr/bin/python3 scripts/tools/gen_db_disk_layout.py --build-dir build-linux [--check]
"""

import argparse
import json
import re
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "database" / "db_load.cpp"
OUTPUT = ROOT / "src" / "database" / "db_disk_layout_gen.h"

# Unions that hold a pointer in one member and plain data in another. The
# converter has one fixed rule per union, chosen so that every member reads back
# correctly: a widened pointer keeps the retail 32-bit value in its low 4 bytes,
# so an int or float member at offset 0 still reads the same value.
UNION_POLICY = {
    # frames (pointer + small union) or frame0 (two shorts at offset 0).
    "XAnimDeltaPartQuatData": "frames",
    # frames (mins, size, pointer, small union) or frame0 (three floats at 0).
    "XAnimPartTransData": "frames",
    # int, float or string pointer, all at offset 0.
    "operandInternalDataUnion": "string",
    # literal pointer or 4-byte code constant / sampler / hash at offset 0.
    "MaterialArgumentDef": "literalConst",
    # op (int at 0) or operand (int + data union).
    "entryInternalData": "operand",
    # child[2] pointers, or s (int nodeCount, pointer nodes): the same slots.
    "pathnode_tree_info_t": "child",
    # loadSnd pointer, or streamSnd (two pointers) starting at the same slot.
    "SoundFileRef": "streamSnd",
    # leaf (one pointer) or children (12 bytes of data). Copied as children;
    # Load_cLeafBrushNode widens the leaf pointer when the node is a leaf.
    "cLeafBrushNodeData_t": "children",
}

# Headers the generated file needs; the same set db_load.cpp includes.
INCLUDES = """#include <universal/q_shared.h>
#include "database.h"

#include <xanim/xanim.h>
#include <xanim/xmodel.h>
#include <sound/snd_local.h>
#include <gfx_d3d/fxprimitives.h>
#include <gfx_d3d/r_material.h>

enum weapPositionAnimNum_t : int;
enum weapAnimFiles_t : int;
enum weaponAltModel_t : int;
#include <gfx_d3d/r_gfx.h>
#include <xanim/dobj.h>
#include <gfx_d3d/r_buffers.h>
#include <DynEntity/DynEntity_client.h>
#include <gfx_d3d/r_water.h>
#include <gfx_d3d/r_image.h>
#include <universal/com_sndalias.h>
#include <gfx_d3d/r_sky.h>
#include <gfx_d3d/r_primarylights.h>
#include <game/g_bsp.h>
"""


class GenError(Exception):
    pass


def load_clang():
    try:
        import clang.cindex as ci
    except ImportError as error:
        raise GenError("the libclang Python bindings are missing: install python3-clang-18 "
                       "and run this script with /usr/bin/python3") from error
    if not ci.Config.loaded:
        for candidate in ("/usr/lib/llvm-18/lib/libclang-18.so.1", "/usr/lib/llvm-18/lib/libclang.so.1"):
            if Path(candidate).exists():
                ci.Config.set_library_file(candidate)
                break
    return ci


def compile_args(build_dir):
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise GenError(f"{database} not found: configure an i386 Linux build first")
    for entry in json.loads(database.read_text(encoding="utf-8")):
        if Path(entry["file"]).resolve() == SOURCE:
            argv = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
            break
    else:
        raise GenError(f"{SOURCE} is not in {database}")
    args, skip = [], False
    for arg in argv[1:]:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-c"):
            skip = True
            continue
        if arg.endswith(".cpp") or arg in ("-m32", "-m64", "-g") or arg.startswith("-W"):
            continue
        args.append(arg)
    return args


class Layouts:
    def __init__(self, ci, args):
        self.ci = ci
        self.tus = {}
        for arch in ("-m32", "-m64"):
            index = ci.Index.create()
            resource = Path("/usr/lib/llvm-18/lib/clang/18/include")
            extra = ["-isystem", str(resource)] if resource.is_dir() else []
            tu = index.parse(str(SOURCE), args=args + [arch] + extra)
            errors = [str(d) for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
            if errors:
                raise GenError(f"Clang could not parse {SOURCE.name} with {arch}:\n  " + "\n  ".join(errors[:10]))
            self.tus[arch] = tu

    def stream_types(self, arch):
        ci = self.ci
        found = []
        for cursor in self.tus[arch].cursor.get_children():
            if (cursor.kind == ci.CursorKind.VAR_DECL and cursor.spelling.startswith("var")
                    and cursor.location.file and Path(cursor.location.file.name).resolve() == SOURCE):
                pointee = cursor.type.get_canonical().get_pointee()
                found.append((cursor.spelling, pointee.get_canonical()))
        return found


def fields(ci, record_type):
    decl = record_type.get_canonical().get_declaration()
    return [c for c in decl.get_children() if c.kind == ci.CursorKind.FIELD_DECL]


def is_union(ci, t):
    return t.get_canonical().get_declaration().kind == ci.CursorKind.UNION_DECL


def type_name(t):
    name = t.get_canonical().spelling
    for prefix in ("struct ", "union ", "enum ", "class "):
        if name.startswith(prefix):
            name = name[len(prefix):]
    if "anonymous" in name or "unnamed" in name:
        raise GenError(f"cannot name type {t.spelling!r}")
    return name


class Leaf:
    __slots__ = ("kind", "disk", "native", "size", "path")

    def __init__(self, kind, disk, native, size, path):
        self.kind, self.disk, self.native, self.size, self.path = kind, disk, native, size, path


def join(path, member):
    if not member:
        return path
    return f"{path}.{member}" if path else member


class Flattener:
    def __init__(self, ci):
        self.ci = ci
        self.plain_cache = {}

    def has_pointer(self, t):
        ci = self.ci
        t = t.get_canonical()
        if t.kind in (ci.TypeKind.POINTER, ci.TypeKind.MEMBERPOINTER):
            return True
        if t.kind == ci.TypeKind.CONSTANTARRAY:
            return self.has_pointer(t.element_type)
        if t.kind == ci.TypeKind.RECORD:
            return any(self.has_pointer(f.type) for f in fields(ci, t))
        return False

    def plain(self, t32, t64):
        """No pointers, and the same size and field offsets on both targets."""
        ci = self.ci
        t32, t64 = t32.get_canonical(), t64.get_canonical()
        key = t32.spelling
        if key in self.plain_cache:
            return self.plain_cache[key]
        result = t32.get_size() == t64.get_size() and not self.has_pointer(t32)
        if result and t32.kind == ci.TypeKind.CONSTANTARRAY:
            result = self.plain(t32.element_type, t64.element_type)
        elif result and t32.kind == ci.TypeKind.RECORD:
            f32, f64 = fields(ci, t32), fields(ci, t64)
            result = len(f32) == len(f64) and all(
                a.get_field_offsetof() == b.get_field_offsetof() and self.plain(a.type, b.type)
                for a, b in zip(f32, f64))
        self.plain_cache[key] = result
        return result

    def flatten(self, t32, t64, disk, native, path, out, top):
        """Append leaves for one object of type t at the given byte offsets."""
        ci = self.ci
        t32, t64 = t32.get_canonical(), t64.get_canonical()
        size32 = t32.get_size()
        if self.plain(t32, t64):
            out.append(Leaf("copy", disk, native, size32, path))
            return
        if t32.kind in (ci.TypeKind.POINTER, ci.TypeKind.MEMBERPOINTER):
            if size32 != 4 or t64.get_size() not in (4, 8):
                raise GenError(f"{top}: pointer {path} is {size32} bytes on disk")
            out.append(Leaf("ptr", disk, native, 4, path))
            return
        if t32.kind == ci.TypeKind.CONSTANTARRAY:
            e32, e64 = t32.element_type, t64.element_type
            s32, s64 = e32.get_size(), e64.get_size()
            for i in range(t32.element_count):
                self.flatten(e32, e64, disk + i * s32, native + i * s64, f"{path}[{i}]", out, top)
            return
        if t32.kind != ci.TypeKind.RECORD:
            raise GenError(f"{top}: {path} ({t32.spelling}) is {size32} bytes on disk and "
                           f"{t64.get_size()} natively")
        f32, f64 = fields(ci, t32), fields(ci, t64)
        if len(f32) != len(f64):
            raise GenError(f"{top}: {t32.spelling} has different fields on the two targets")
        if is_union(ci, t32):
            self.flatten_union(t32, t64, f32, f64, disk, native, path, out, top)
            return
        for a, b in zip(f32, f64):
            if a.is_bitfield():
                raise GenError(f"{top}: bit-field {join(path, a.spelling)} in a record that needs converting")
            self.flatten(a.type, b.type, disk + a.get_field_offsetof() // 8, native + b.get_field_offsetof() // 8,
                         join(path, a.spelling), out, top)

    def flatten_union(self, t32, t64, f32, f64, disk, native, path, out, top):
        name = type_name(t32)
        members = list(zip(f32, f64))
        chosen = None
        if name in UNION_POLICY:
            chosen = [m for m in members if m[0].spelling == UNION_POLICY[name]]
            if not chosen:
                raise GenError(f"{top}: union {name} has no member {UNION_POLICY[name]!r}")
            chosen = chosen[0]
        elif len(members) == 1:
            chosen = members[0]
        else:
            # A union whose members are all single pointers (or unions of them).
            for a, b in members:
                leaves = []
                self.flatten(a.type, b.type, 0, 0, "", leaves, top)
                if not (len(leaves) == 1 and leaves[0].kind == "ptr" and leaves[0].disk == 0):
                    raise GenError(f"{top}: union {name} at {path or '(top)'} mixes pointers and data; "
                                   f"add it to UNION_POLICY in {Path(__file__).name}")
            chosen = members[0]
        a, b = chosen
        leaves = []
        self.flatten(a.type, b.type, disk, native, join(path, a.spelling), leaves, top)
        covered = sum(leaf.size for leaf in leaves)
        if covered > t32.get_size():
            raise GenError(f"{top}: union {name} member {a.spelling} overruns the union")
        out.extend(leaves)


def merge(leaves):
    """Merge neighbouring copies that keep the same disk-to-native distance."""
    ops = []
    for leaf in sorted(leaves, key=lambda leaf: leaf.disk):
        if (ops and leaf.kind == "copy" and ops[-1].kind == "copy"
                and leaf.native - leaf.disk == ops[-1].native - ops[-1].disk
                and leaf.disk >= ops[-1].disk + ops[-1].size):
            last = ops[-1]
            last.size = leaf.disk + leaf.size - last.disk
            continue
        ops.append(Leaf(leaf.kind, leaf.disk, leaf.native, leaf.size, leaf.path))
    for a, b in zip(ops, ops[1:]):
        if b.disk < a.disk + a.size:
            raise GenError(f"overlapping operations at disk offsets {a.disk} and {b.disk}")
    return ops


STREAM_SOURCES = (SOURCE, ROOT / "src" / "database" / "db_file_load.cpp")
SIZEOF_IN_LOAD_STREAM = re.compile(r"\bLoad_Stream\s*\([^;]*\bsizeof\s*\(")


def check_stream_sizes():
    """Load_Stream sizes are retail disk byte counts, so sizeof (a native size) is wrong."""
    for path in STREAM_SOURCES:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if SIZEOF_IN_LOAD_STREAM.search(line):
                raise GenError(f"{path.relative_to(ROOT)}:{number}: Load_Stream takes the retail disk "
                               "size; sizeof gives the native size, which differs on 64-bit builds")


def generate(build_dir):
    check_stream_sizes()
    ci = load_clang()
    layouts = Layouts(ci, compile_args(build_dir))
    t64_by_var = dict(layouts.stream_types("-m64"))
    flattener = Flattener(ci)
    plain, converted, seen = [], [], set()
    for var, t32 in layouts.stream_types("-m32"):
        t64 = t64_by_var[var]
        if t32.kind == ci.TypeKind.VOID or t32.kind in (ci.TypeKind.POINTER, ci.TypeKind.MEMBERPOINTER):
            continue  # raw bytes, or a pointer slot: handled by the runtime templates
        if t32.get_size() < 0:
            continue  # declared but never defined, so never streamed
        decl = t32.get_declaration()
        if decl.location.file and Path(decl.location.file.name).resolve() == SOURCE:
            continue  # local to db_load.cpp; only that file could stream it
        name = type_name(t32)
        if name in seen:
            continue
        seen.add(name)
        if flattener.plain(t32, t64):
            plain.append((name, t32.get_size(), var))
            continue
        leaves = []
        flattener.flatten(t32, t64, 0, 0, "", leaves, name)
        ops = merge(leaves)
        converted.append((name, t32.get_size(), t32.get_align(), t64.get_size(), var, ops))
    return render(plain, converted)


def render(plain, converted):
    lines = [
        "// Generated by scripts/tools/gen_db_disk_layout.py from src/database/db_load.cpp.",
        "// Do not edit: rerun the script after changing a streamed asset struct.",
        "//",
        "// Retail (32-bit) record layouts of every type the fastfile loader streams,",
        "// and where each piece lives in the native struct. See db_disk_layout.h.",
        "#pragma once",
        "",
        INCLUDES.rstrip(),
        "",
        '#include "db_disk_layout.h"',
        "",
        "// Types whose layout is the same on every target and holds no pointer.",
    ]
    for name, size, var in plain:
        lines.append(f"DB_DISK_LAYOUT_PLAIN({name}, {size}); // {var}")
    lines.append("")
    lines.append("// Types that hold pointers or change size: converted field by field.")
    for name, size, align, size64, var, ops in converted:
        lines.append(f"// {var}: {size} bytes on disk, {size64} on Linux x86_64")
        lines.append(f"DB_DISK_LAYOUT_BEGIN({name}, {size}, {align})")
        for op in ops:
            member = op.path
            native = f"offsetof({name}, {member})" if member else "0"
            kind = "DB_DISK_PTR" if op.kind == "ptr" else "DB_DISK_COPY"
            lines.append(f"    {{ {kind}, {op.disk}, {native}, {op.size} }},")
        lines.append(f"DB_DISK_LAYOUT_END({name})")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-linux",
                        help="configured Linux build directory with compile_commands.json")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true",
                        help="fail if the checked-in file differs from a fresh generation")
    args = parser.parse_args()
    try:
        text = generate(args.build_dir.resolve())
    except GenError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        if current != text:
            print(f"FAIL: {args.output} is stale; rerun {Path(__file__).name}", file=sys.stderr)
            return 1
        print(f"PASS: {args.output.relative_to(ROOT)} matches the struct definitions")
        return 0
    args.output.write_text(text, encoding="utf-8")
    print(f"wrote {args.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
