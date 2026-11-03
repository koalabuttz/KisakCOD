#!/usr/bin/env python3
"""Find pointer arithmetic that negates a 32-bit unsigned value.

Decompiled code often reads ``&top[-count]`` where ``count`` is uint32_t.
``-count`` is then an unsigned 32-bit value (2^32 - count). On i386 the pointer
wraps back to the intended element; on x86_64 the index is zero-extended and
the pointer lands about 2^32 elements past the array. Write ``top - count``
instead.

This parses every source in a build's compile_commands.json with libclang and
reports a unary minus on an ``unsigned int`` operand whose value is used as an
array index or added to a pointer. Narrower unsigned types are promoted to int
before the minus, and 64-bit unsigned values wrap at pointer width, so neither
is reported.

Usage (needs python3-clang-18, run with the system Python):
    /usr/bin/python3 scripts/tools/check_unsigned_negation.py --build-dir build-x64
"""

import argparse
import json
from multiprocessing import Pool
import os
from pathlib import Path
import shlex
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"


def load_clang():
    try:
        import clang.cindex as ci
    except ImportError:
        sys.exit("the libclang Python bindings are missing: install python3-clang-18 "
                 "and run this script with /usr/bin/python3")
    if not ci.Config.loaded:
        for candidate in ("/usr/lib/llvm-18/lib/libclang-18.so.1", "/usr/lib/llvm-18/lib/libclang.so.1"):
            if Path(candidate).exists():
                ci.Config.set_library_file(candidate)
                break
    return ci


def translation_units(build_dir: Path):
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        sys.exit(f"{database} not found: configure the build with CMAKE_EXPORT_COMPILE_COMMANDS=ON")
    seen = set()
    for entry in json.loads(database.read_text(encoding="utf-8")):
        path = Path(entry["directory"], entry["file"]).resolve()
        if path in seen or SRC not in path.parents:
            continue
        seen.add(path)
        argv = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
        args, skip = [], False
        for arg in argv[1:]:
            if skip:
                skip = False
                continue
            if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
                skip = True
                continue
            if arg in ("-MD", "-MMD", "-g") or Path(arg).name == path.name:
                continue
            args.append(arg)
        yield str(path), args, entry["directory"]


def scan(job):
    path, args, directory = job
    ci = load_clang()
    os.chdir(directory)
    tu = ci.Index.create().parse(path, args=args + ["-w"])
    errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
    if errors:
        return path, [], [str(errors[0])]
    K = ci.CursorKind
    passthrough = {K.UNEXPOSED_EXPR, K.PAREN_EXPR}
    hits = []

    def pointer_use(stack):
        # stack[-1] is the unary minus; climb through implicit casts and parens.
        i = len(stack) - 2
        while i >= 0 and stack[i].kind in passthrough:
            i -= 1
        if i < 0:
            return False
        parent = stack[i]
        if parent.kind == K.ARRAY_SUBSCRIPT_EXPR:
            return True
        if parent.kind in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR):
            return parent.type.get_canonical().kind == ci.TypeKind.POINTER
        return False

    def walk(cursor, stack):
        for child in cursor.get_children():
            loc = child.location
            if loc.file is not None and loc.file.name != path:
                continue
            stack.append(child)
            if child.kind == K.UNARY_OPERATOR:
                tokens = list(child.get_tokens())
                operands = list(child.get_children())
                if tokens and tokens[0].spelling == "-" and operands:
                    operand = operands[0].type.get_canonical().kind
                    if operand == ci.TypeKind.UINT and pointer_use(stack):
                        source = Path(path).read_text(errors="replace").splitlines()[loc.line - 1].strip()
                        hits.append(f"{Path(path).relative_to(ROOT)}:{loc.line}:{loc.column}: {source}")
            walk(child, stack)
            stack.pop()

    walk(tu.cursor, [])
    return path, hits, []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()
    load_clang()
    jobs = list(translation_units(args.build_dir.resolve()))
    hits, failures = [], []
    with Pool(args.jobs) as pool:
        for path, found, errors in pool.imap_unordered(scan, jobs):
            hits.extend(found)
            failures.extend(f"{path}: {e}" for e in errors)
    for line in sorted(set(hits)):
        print(f"NEGATED UNSIGNED INDEX: {line}")
    for line in failures:
        print(f"PARSE ERROR: {line}")
    if failures:
        return 2
    if hits:
        print(f"{len(set(hits))} negated 32-bit unsigned index(es); write `ptr - n` instead of `&ptr[-n]`")
        return 1
    print(f"no negated 32-bit unsigned indexes in {len(jobs)} translation units")
    return 0


if __name__ == "__main__":
    sys.exit(main())
