#!/usr/bin/env python3
"""Find integer literals that spell a struct's 32-bit size where the size differs at 64 bits.

Decompiled code often writes a struct's size as the number the i386 binary
used: ``R_GetCommandBuffer(RC_DRAW_TEXT_2D, (len + 84) & ~3)`` where 84 is
sizeof(GfxCmdDrawText2D) on i386. A struct that holds a pointer is larger on
x86_64, so the literal allocates, copies or strides too few bytes there.

This parses every source in a build's compile_commands.json twice with
libclang, once as i386 (-m32) and once as x86_64 (-m64). In each statement it
looks at the record types the statement's expressions use (through pointers,
arrays and references) and reports an integer literal of at least --min-size
(or a 4 that multiplies a count, as in ``Alloc(4 * n)``) that equals one of
those records' i386 size when the record's x86_64 size is different. Write sizeof(T) instead, or assert the literal with LAYOUT_32BIT.

It also reports an allocation cast to a pointer-to-pointer type whose size is
a multiple of 4 written without sizeof, such as `(T **)Alloc(4 * n)` or
`(T **)Alloc(4096)`: an array of pointers sized for 4-byte pointers.

A literal can match a size by chance (flags, counts, array indexes); each
report is a place to read, and the allow file lists the ones read and found to
be something other than a size, by file and source text. Calls to
track_static_alloc_internal are skipped: it records nothing (its body is
compiled out), so its size argument is never used.

Usage (needs python3-clang-18, run with the system Python):
    /usr/bin/python3 scripts/tools/check_32bit_size_literals.py --build-dir build-x64c
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
DEFAULT_ALLOW = ROOT / "scripts" / "tools" / "check_32bit_size_literals_allow.txt"


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
            if arg in ("-MD", "-MMD", "-g", "-m32", "-m64") or Path(arg).name == path.name:
                continue
            args.append(arg)
        yield str(path), args, entry["directory"]


def literal_value(cursor):
    tokens = list(cursor.get_tokens())
    if len(tokens) != 1:
        return None
    text = tokens[0].spelling.rstrip("uUlL")
    try:
        return int(text, 0)
    except ValueError:
        return None


def record_types(ci, cursor, out):
    """Collect the complete record types an expression subtree names, with their sizes."""
    K = ci.CursorKind
    T = ci.TypeKind
    stack = [cursor]
    while stack:
        node = stack.pop()
        if node.kind.is_expression() or node.kind in (K.VAR_DECL, K.TYPE_REF):
            t = node.type.get_canonical()
            for _ in range(8):
                if t.kind in (T.POINTER, T.LVALUEREFERENCE, T.RVALUEREFERENCE):
                    t = t.get_pointee().get_canonical()
                elif t.kind in (T.CONSTANTARRAY, T.INCOMPLETEARRAY):
                    t = t.element_type.get_canonical()
                else:
                    break
            if t.kind == T.RECORD:
                size = t.get_size()
                if size > 0:
                    out[t.spelling] = size
        stack.extend(node.get_children())


SKIPPED_CALLS = {"track_static_alloc_internal"}

# db_load.cpp reads fastfile records at their retail (i386) sizes on purpose;
# db_stream.cpp converts them for x86_64 (docs/linux-x64.md, "Fastfile records").
RETAIL_RECORD_SOURCES = {"src/database/db_load.cpp"}


def raw_asset_loader(path: str) -> bool:
    # *_load_obj.cpp load raw assets, which only tools do (useFastFile 0);
    # --include-raw-loaders scans them too.
    return path.endswith("_load_obj.cpp")


def first_token(cursor):
    for token in cursor.get_tokens():
        return token.spelling
    return None


def scan_arch(ci, path, args, arch, min_size):
    tu = ci.Index.create().parse(path, args=args + [arch, "-w"])
    errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
    if errors:
        return None, str(errors[0])
    K = ci.CursorKind
    # statement extent -> {record: size}
    statements = {}

    def visit(cursor, statement, multiplied=False):
        # A literal below --min-size still counts as a size when it multiplies a
        # count, as in Alloc(4 * n) for an array of 4-byte (i386) records.
        if cursor.kind == K.BINARY_OPERATOR:
            # n * sizeof(T): the 4 is a count, not a size.
            child_multiplied = is_multiply(cursor) and not any(
                c.kind == K.CXX_UNARY_EXPR for c in cursor.get_children())
        elif cursor.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR):
            child_multiplied = multiplied
        else:
            child_multiplied = False
        for child in cursor.get_children():
            loc = child.location
            if loc.file is not None and loc.file.name != path:
                continue
            inner = statement
            if cursor.kind == K.COMPOUND_STMT or (statement is None and child.kind.is_statement()):
                inner = child
            if inner is not None and inner is not statement and first_token(inner) in SKIPPED_CALLS:
                continue
            if child.kind == K.INTEGER_LITERAL and inner is not None:
                value = literal_value(child)
                if value is not None and (value >= min_size or (child_multiplied and value == 4)):
                    key = (inner.extent.start.offset, inner.extent.end.offset)
                    if key not in statements:
                        types = {}
                        record_types(ci, inner, types)
                        statements[key] = types
                    yield "size", loc.line, loc.column, value, statements[key]
            if child.kind == K.CSTYLE_CAST_EXPR:
                value = pointer_array_literal(ci, child)
                if value is not None:
                    yield "pointers", loc.line, loc.column, value, None
            yield from visit(child, inner, child_multiplied)

    found = []
    for kind, line, column, value, types in visit(tu.cursor, None):
        found.append((kind, line, column, value, dict(types or {})))
    return found, None


def is_multiply(node):
    """True for a binary `*`: the operator is the token between the two operands."""
    children = list(node.get_children())
    if len(children) != 2:
        return False
    start, end = children[0].extent.end.offset, children[1].extent.start.offset
    return any(t.spelling == "*" for t in node.get_tokens() if start <= t.extent.start.offset < end)


def pointer_array_literal(ci, cast):
    """`(T **)Alloc(4 * n)` or `(T **)Alloc(4096)`: an array of pointers sized in
    4-byte entries. Returns the literal, or None when the cast is something else
    or the size is written with sizeof."""
    K = ci.CursorKind
    T = ci.TypeKind
    target = cast.type.get_canonical()
    if target.kind != T.POINTER or target.get_pointee().get_canonical().kind != T.POINTER:
        return None
    call = None
    stack = list(cast.get_children())
    while stack:
        node = stack.pop()
        if node.kind == K.CALL_EXPR:
            call = node
            break
        if node.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR):
            stack.extend(node.get_children())
    if call is None:
        return None
    literal = None
    for argument in list(call.get_arguments()):
        stack = [(argument, False)]
        while stack:
            node, multiplied = stack.pop()
            if node.kind == K.CXX_UNARY_EXPR:  # sizeof / alignof
                return None
            if node.kind == K.INTEGER_LITERAL and literal is None:
                value = literal_value(node)
                # A bare 4 is an alignment argument; 4 counts only as `4 * n`.
                if value is not None and value % 4 == 0 and (value > 4 or (value == 4 and multiplied)):
                    literal = value
            is_mul = node.kind == K.BINARY_OPERATOR and any(
                t.spelling == "*" for t in node.get_tokens()
                if t.extent.start.offset not in [c.extent.start.offset for c in node.get_children()])
            if node.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR):
                stack.extend((c, multiplied) for c in node.get_children())
            else:
                stack.extend((c, is_mul) for c in node.get_children())
        if literal is not None:
            return literal
    return None


def scan(job):
    path, args, directory, min_size = job
    ci = load_clang()
    os.chdir(directory)
    narrow, error = scan_arch(ci, path, args, "-m32", min_size)
    if error:
        return path, [], [error]
    wide, error = scan_arch(ci, path, args, "-m64", min_size)
    if error:
        return path, [], [error]
    wide_sizes = {}
    for _, _, _, _, types in wide:
        wide_sizes.update(types)
    hits = []
    source = Path(path).read_text(errors="replace").splitlines()
    rel = Path(path).relative_to(ROOT) if ROOT in Path(path).parents else Path(path)
    for kind, line, column, value, types in narrow:
        text = " ".join(source[line - 1].split())
        if kind == "pointers":
            hits.append((f"{rel}: {text}", f"{rel}:{line}:{column}: pointer array sized with {value} "
                         f"(4-byte pointers; write n * sizeof(T *)): {text}"))
            continue
        for record, size in types.items():
            if size == value and wide_sizes.get(record, size) != size:
                hits.append((f"{rel}: {text}", f"{rel}:{line}:{column}: {value} == i386 sizeof({record}) "
                             f"(x86_64 {wide_sizes[record]}): {text}"))
    return path, hits, []


def load_allow(path: Path):
    """Entries are `path: source line`, whitespace collapsed; `#` starts a comment line."""
    allowed = set()
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.lstrip().startswith("#"):
                continue
            line = " ".join(line.split())
            if line:
                allowed.add(line)
    return allowed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--min-size", type=int, default=8)
    parser.add_argument("--allow", type=Path, default=DEFAULT_ALLOW,
                        help="file of `path: source line` entries read and found not to be a struct size")
    parser.add_argument("--only", nargs="*", default=None, help="scan only sources whose path contains one of these")
    parser.add_argument("--include-raw-loaders", action="store_true",
                        help="also scan the raw asset loaders (*_load_obj.cpp), which the game never runs")
    args = parser.parse_args()
    load_clang()
    allowed = load_allow(args.allow)
    jobs = [(p, a, d, args.min_size) for p, a, d in translation_units(args.build_dir.resolve())
            if (args.only is None or any(o in p for o in args.only))
            and str(Path(p).relative_to(ROOT)) not in RETAIL_RECORD_SOURCES
            and (args.include_raw_loaders or not raw_asset_loader(p))]
    hits, failures, used = {}, [], set()
    with Pool(args.jobs) as pool:
        for path, found, errors in pool.imap_unordered(scan, jobs):
            for key, text in found:
                if key in allowed:
                    used.add(key)
                    continue
                hits[text] = key
            failures.extend(f"{path}: {e}" for e in errors)
    for text in sorted(hits):
        print(f"32-BIT SIZE LITERAL: {text}")
    for line in failures:
        print(f"PARSE ERROR: {line}")
    scanned = {str(Path(p).relative_to(ROOT)) for p, _, _, _ in jobs}
    stale = sorted(a for a in allowed - used if a.split(":", 1)[0] in scanned)
    for key in stale:
        print(f"STALE ALLOW ENTRY: {key}")
    if failures:
        return 2
    if stale:
        print(f"{len(stale)} allow entr(ies) match no reported line; remove them from {args.allow}")
        return 1
    if hits:
        print(f"{len(hits)} literal(s) equal a struct's i386 size that differs on x86_64; "
              "write sizeof(T), or list the line in the allow file if it is not a size")
        return 1
    print(f"no 32-bit struct size literals in {len(jobs)} translation units")
    return 0


if __name__ == "__main__":
    sys.exit(main())
