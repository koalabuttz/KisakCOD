#!/usr/bin/env python3
"""Find byte offsets and type puns that are right for the i386 layout but not for x86_64.

Decompiled code often reaches a struct field through a byte offset that the
i386 binary used, instead of through the field:

    *(float *)((char *)context + 28)          a field at i386 offset 28
    (VehicleTags *)((char *)&veh->boneIndex + 12 * i)
    *(VariableUnion *)((char *)&hud->elem + ofs) = value   4-byte field, 8-byte store

A struct that holds a pointer (directly or in a member) has a different layout
on x86_64, so those offsets land on another field there. This parses each
source in a build's compile_commands.json twice with libclang, as i386 (-m32)
and as x86_64 (-m64), and reports:

offset  a pointer to a record (or the address of a member) cast to another
        pointer or integer type and moved by a constant: the field at that
        i386 byte offset sits elsewhere on x86_64, or the access straddles it.
stride  the same base moved by `K * n` where K is the i386 size of a record
        (or array element) inside it whose x86_64 size differs.
spill   a member array (or the address of a member) indexed by a constant past
        its own end, reaching a neighbouring field whose x86_64 position is
        not where the same index lands there.
pun     a pointer cast from U* to T* where U and T have the same i386 size but
        different x86_64 sizes, such as (int *)&pointerField or
        (VariableUnion *)&int32Field.

Each report is a place to read; the allow file lists lines read and found to
be fine, by file and source text (same format as check_32bit_size_literals).

Usage (needs python3-clang-18, run with the system Python):
    /usr/bin/python3 scripts/tools/check_32bit_layout_offsets.py --build-dir build-x64c
"""

import argparse
from multiprocessing import Pool
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_32bit_size_literals import (  # noqa: E402
    ROOT, RETAIL_RECORD_SOURCES, load_allow, load_clang, raw_asset_loader, translation_units)

DEFAULT_ALLOW = ROOT / "scripts" / "tools" / "check_32bit_layout_offsets_allow.txt"


class Arch:
    """One parse of a translation unit, with layout helpers."""

    def __init__(self, ci, path, args, flag):
        self.ci = ci
        self.K = ci.CursorKind
        self.T = ci.TypeKind
        self.path = path
        self.tu = ci.Index.create().parse(path, args=args + [flag, "-w"])
        self.errors = [d for d in self.tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        self.records = {}

    def record_key(self, t):
        t = t.get_canonical()
        decl = t.get_declaration()
        if t.kind != self.T.RECORD or decl is None:
            return None
        spelling = t.spelling.replace("const ", "").replace("volatile ", "")
        self.records.setdefault(spelling, t)
        return spelling

    def fields(self, t):
        t = t.get_canonical()
        try:
            return list(t.get_fields())
        except Exception:
            return []

    def size(self, t):
        return t.get_canonical().get_size()

    def path_at(self, t, offset, depth=0):
        """Field path (list of (field index, array index)) covering byte `offset` of record t."""
        if depth > 12:
            return []
        out = []
        for index, field in enumerate(self.fields(t)):
            bits = field.get_field_offsetof()
            if bits < 0:
                continue
            start = bits // 8
            ft = field.type.get_canonical()
            size = ft.get_size()
            if size <= 0 or not (start <= offset < start + size):
                continue
            rest = offset - start
            if ft.kind == self.T.CONSTANTARRAY:
                elem = ft.element_type.get_canonical()
                esize = elem.get_size()
                if esize <= 0:
                    return [(index, None)]
                out = [(index, rest // esize)]
                rest %= esize
                if elem.kind == self.T.RECORD:
                    out += self.path_at(elem, rest, depth + 1)
                elif rest:
                    out.append(("+", rest))
                return out
            if ft.kind == self.T.RECORD:
                return [(index, None)] + self.path_at(ft, rest, depth + 1)
            out = [(index, None)]
            if rest:
                out.append(("+", rest))
            return out
        return [("?", offset)]

    def offset_of(self, t, path):
        """Byte offset of a field path in record t, or None."""
        total = 0
        for index, sub in path:
            if index == "+":
                total += sub
                continue
            if index == "?":
                return None
            fields = self.fields(t)
            if index >= len(fields):
                return None
            field = fields[index]
            total += field.get_field_offsetof() // 8
            ft = field.type.get_canonical()
            if sub is not None:
                if ft.kind != self.T.CONSTANTARRAY:
                    return None
                elem = ft.element_type.get_canonical()
                total += sub * elem.get_size()
                t = elem
            else:
                t = ft
        return total

    def path_name(self, t, path):
        names = []
        for index, sub in path:
            if index == "+":
                names.append(f"+{sub}")
                continue
            if index == "?":
                names.append(f"<past end +{sub}>")
                continue
            fields = self.fields(t)
            if index >= len(fields):
                names.append("?")
                break
            field = fields[index]
            names.append((field.spelling or "<anon>") + (f"[{sub}]" if sub is not None else ""))
            ft = field.type.get_canonical()
            t = ft.element_type.get_canonical() if sub is not None and ft.kind == self.T.CONSTANTARRAY else ft
        return ".".join(names)

    def sizes_inside(self, t, depth=0, out=None):
        """{type spelling: size} for t and every record/array element type inside it."""
        if out is None:
            out = {}
        t = t.get_canonical()
        if depth > 12:
            return out
        if t.kind == self.T.RECORD:
            out[t.spelling] = t.get_size()
            for field in self.fields(t):
                self.sizes_inside(field.type, depth + 1, out)
        elif t.kind == self.T.CONSTANTARRAY:
            elem = t.element_type.get_canonical()
            out["elem " + elem.spelling] = elem.get_size()
            self.sizes_inside(elem, depth + 1, out)
        return out


def strip(K, node):
    while node is not None and node.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR):
        children = list(node.get_children())
        if len(children) != 1:
            break
        node = children[0]
    return node


def operator_token(node):
    children = list(node.get_children())
    if len(children) != 2:
        return None
    start, end = children[0].extent.end.offset, children[1].extent.start.offset
    for t in node.get_tokens():
        if start <= t.extent.start.offset < end and t.spelling in ("+", "-", "*", "+=", "-="):
            return t.spelling
    return None


def const_value(K, node):
    """Integer value of a constant expression made of literals, + - * and parens."""
    tokens = [t.spelling for t in node.get_tokens()]
    if not tokens:
        return None
    text = []
    for tok in tokens:
        if tok in "+-*()":
            text.append(tok)
            continue
        raw = tok.rstrip("uUlL")
        try:
            text.append(str(int(raw, 0)))
        except ValueError:
            return None
    try:
        return int(eval(" ".join(text), {"__builtins__": {}}))  # digits and + - * ( ) only
    except Exception:
        return None


def is_cast(K, node):
    return node.kind in (K.CSTYLE_CAST_EXPR, K.CXX_REINTERPRET_CAST_EXPR, K.CXX_STATIC_CAST_EXPR,
                         K.CXX_FUNCTIONAL_CAST_EXPR)


def cast_operand(K, node):
    children = [c for c in node.get_children() if c.kind != K.TYPE_REF and not c.kind.is_reference()]
    return strip(K, children[-1]) if children else None


class Scanner:
    def __init__(self, arch):
        self.a = arch
        self.K = arch.K
        self.T = arch.T

    def base_of(self, node):
        """For a cast operand: (record type, byte offset in it) when the operand points into a record."""
        K, T = self.K, self.T
        node = strip(K, node)
        if node is None:
            return None
        t = node.type.get_canonical()
        # &x.f / &x->f / &x.f[c]
        if node.kind == K.UNARY_OPERATOR:
            tokens = list(node.get_tokens())
            if tokens and tokens[0].spelling == "&":
                inner = strip(K, list(node.get_children())[0])
                return self.member_location(inner)
            return None
        # array member decaying to a pointer
        if node.kind == K.MEMBER_REF_EXPR and t.kind == T.CONSTANTARRAY:
            return self.member_location(node)
        if t.kind == T.POINTER:
            pointee = t.get_pointee().get_canonical()
            if pointee.kind == T.RECORD and pointee.get_size() > 0:
                return pointee, 0
        return None

    def member_location(self, node):
        """(record, offset) for a member expression x.f, x->f, x.f[c] (constant c)."""
        K, T = self.K, self.T
        node = strip(K, node)
        extra = 0
        if node is not None and node.kind == K.ARRAY_SUBSCRIPT_EXPR:
            children = list(node.get_children())
            if len(children) != 2:
                return None
            idx = const_value(K, children[1])
            if idx is None:
                return None
            elem = node.type.get_canonical().get_size()
            extra = idx * elem
            node = strip(K, children[0])
        if node is None or node.kind != K.MEMBER_REF_EXPR:
            return None
        field = node.referenced
        if field is None or field.kind != K.FIELD_DECL:
            return None
        parent = field.semantic_parent
        if parent is None:
            return None
        rt = parent.type.get_canonical()
        if rt.kind != T.RECORD or rt.get_size() <= 0:
            return None
        bits = field.get_field_offsetof()
        if bits < 0:
            return None
        return rt, bits // 8 + extra

    def walk(self):
        K = self.K
        found = []
        stack = [self.a.tu.cursor]
        while stack:
            node = stack.pop()
            loc = node.location
            if loc.file is not None and loc.file.name != self.a.path and node.kind != K.TRANSLATION_UNIT:
                continue
            if node.kind == K.BINARY_OPERATOR or node.kind == K.COMPOUND_ASSIGNMENT_OPERATOR:
                hit = self.arith(node)
                if hit:
                    found.append(hit)
                hit = self.union_copy(node)
                if hit:
                    found.append(hit)
            elif node.kind == K.ARRAY_SUBSCRIPT_EXPR:
                hit = self.subscript(node)
                if hit:
                    found.append(hit)
            if is_cast(K, node):
                hit = self.pun(node)
                if hit:
                    found.append(hit)
            stack.extend(node.get_children())
        return found

    def chain(self, node):
        """Flatten a + / - chain: returns (bottom pointer operand, [(sign, term)])."""
        K = self.K
        terms = []
        while True:
            node = strip(K, node)
            if node is None or node.kind != K.BINARY_OPERATOR:
                break
            op = operator_token(node)
            if op not in ("+", "-"):
                break
            left, right = list(node.get_children())
            lt = strip(K, left).type.get_canonical()
            if lt.kind in (self.T.POINTER,) or is_cast(K, strip(K, left)) or strip(K, left).kind == K.BINARY_OPERATOR:
                terms.append((op, right))
                node = left
            else:
                # integer + pointer: pointer on the right
                terms.append((op, left))
                node = right
        return node, terms

    def classify_terms(self, terms):
        offset, strides, dynamic = 0, [], False
        for sign, term in terms:
            value = const_value(self.K, term)
            if value is not None:
                offset += value if sign == "+" else -value
                continue
            dynamic = True
            node = strip(self.K, term)
            if node is not None and node.kind == self.K.BINARY_OPERATOR and operator_token(node) == "*":
                for child in node.get_children():
                    v = const_value(self.K, child)
                    if v is not None:
                        strides.append(v)
        return offset, strides, dynamic

    def arith(self, node):
        K, T = self.K, self.T
        op = operator_token(node)
        if op not in ("+", "-"):
            return None
        # only the top of a chain
        bottom, terms = self.chain(node)
        bottom = strip(K, bottom)
        if bottom is None or not terms:
            return None
        return self.from_base(node, bottom, terms)

    def subscript(self, node):
        K = self.K
        children = list(node.get_children())
        if len(children) != 2:
            return None
        base = strip(K, children[0])
        return self.from_base(node, base, [("+", children[1])], subscript=True)

    def from_base(self, node, base, terms, subscript=False):
        K, T = self.K, self.T
        offset, strides, dynamic = self.classify_terms(terms)
        if base is None:
            return None
        if is_cast(K, base):
            target = base.type.get_canonical()
            inner = cast_operand(K, base)
            loc = self.base_of(inner) if inner is not None else None
            if loc is None:
                return None
            rt, start = loc
            if target.kind == T.POINTER:
                pointee = target.get_pointee().get_canonical()
                if pointee.kind == T.VOID:
                    unit = 1
                else:
                    unit = pointee.get_size()
                    if unit <= 0:
                        return None
                if pointee.kind == T.RECORD and pointee.spelling == rt.spelling:
                    return None  # moving by whole records of its own type
            elif target.kind in (T.INT, T.UINT, T.LONG, T.ULONG, T.LONGLONG, T.ULONGLONG):
                unit = 1
            else:
                return None
            kind = "offset"
        elif not subscript:
            loc = self.member_location(base) if base.kind != K.UNARY_OPERATOR else self.base_of(base)
            if loc is None:
                return None
            rt, start = loc
            mt = base.type.get_canonical()
            if base.kind == K.UNARY_OPERATOR:
                mt = mt.get_pointee().get_canonical()
                unit = mt.get_size()
            elif mt.kind == T.CONSTANTARRAY:
                unit = mt.element_type.get_canonical().get_size()
            else:
                return None
            kind = "spill"
        else:
            # x.f[c] with x.f an array: spill past its end
            if base.kind == K.MEMBER_REF_EXPR:
                mt = base.type.get_canonical()
                if mt.kind != T.CONSTANTARRAY or dynamic:
                    return None
                count = mt.element_count
                if 0 <= offset < count:
                    return None
                loc = self.member_location(base)
            elif base.kind == K.UNARY_OPERATOR:
                loc = self.base_of(base)
                mt = base.type.get_canonical().get_pointee().get_canonical()
            else:
                return None
            if loc is None:
                return None
            rt, start = loc
            unit = (mt.element_type if mt.kind == T.CONSTANTARRAY else mt).get_canonical().get_size()
            kind = "spill"
        if unit <= 0:
            return None
        if kind == "spill" and not dynamic:
            # inside the member itself is ordinary indexing
            size = self.a.size(mt)
            if 0 <= offset * unit < max(size, unit):
                return None
        if offset == 0 and not strides:
            return None
        rk = self.a.record_key(rt)
        return {
            "line": node.location.line, "column": node.location.column, "kind": kind,
            "record": rk, "start": start, "offset": offset, "unit": unit, "strides": strides,
        }

    def union_member(self, node):
        node = strip(self.K, node)
        if node is None or node.kind != self.K.MEMBER_REF_EXPR:
            return None
        field = node.referenced
        if field is None or field.kind != self.K.FIELD_DECL:
            return None
        parent = field.semantic_parent
        if parent is None or parent.kind != self.K.UNION_DECL:
            return None
        return field, parent

    def union_copy(self, node):
        """a.u.narrow = b.u.narrow: a union copied through one of its members."""
        children = list(node.get_children())
        if len(children) != 2:
            return None
        start, end = children[0].extent.end.offset, children[1].extent.start.offset
        if not any(t.spelling == "=" for t in node.get_tokens() if start <= t.extent.start.offset < end):
            return None
        left = self.union_member(children[0])
        right = self.union_member(children[1])
        if left is None or right is None:
            return None
        if left[1].type.get_canonical().spelling != right[1].type.get_canonical().spelling:
            return None
        field, union = right
        return {
            "line": node.location.line, "column": node.location.column, "kind": "union",
            "union": union.type.spelling, "field": field.spelling,
            "field_size": field.type.get_canonical().get_size(), "union_size": union.type.get_canonical().get_size(),
        }

    def has_sizeof(self, node):
        stack = [node]
        while stack:
            n = stack.pop()
            if n.kind == self.K.CXX_UNARY_EXPR:
                return True
            stack.extend(n.get_children())
        return False

    def pun(self, node):
        K, T = self.K, self.T
        target = node.type.get_canonical()
        if target.kind != T.POINTER:
            return None
        inner = cast_operand(K, node)
        if inner is None:
            return None
        source = inner.type.get_canonical()
        if source.kind != T.POINTER:
            source = None
        tp = target.get_pointee().get_canonical()
        bottom, terms = self.chain(inner)
        bottom = strip(K, bottom)
        if terms and bottom is not None and is_cast(K, bottom) and tp.get_size() > 0:
            bt = bottom.type.get_canonical()
            byte_base = bt.kind in (T.INT, T.UINT, T.LONG, T.ULONG, T.LONGLONG, T.ULONGLONG) or (
                bt.kind == T.POINTER and bt.get_pointee().get_canonical().kind in (
                    T.VOID, T.CHAR_S, T.SCHAR, T.UCHAR, T.CHAR_U))
            known = self.base_of(cast_operand(K, bottom)) if byte_base else None
            if byte_base and tp.kind == T.RECORD and known is not None and known[0].spelling == tp.spelling:
                byte_base = False  # stepping between records of one type
            if byte_base and any(self.has_sizeof(term) for _, term in terms):
                byte_base = False  # sized with sizeof: follows the build's layout
            if byte_base:
                return {
                    "line": node.location.line, "column": node.location.column, "kind": "width",
                    "to": tp.spelling, "to_size": tp.get_size(),
                }
        if source is None:
            return None
        sp = source.get_pointee().get_canonical()
        if tp.kind in (T.VOID, T.CHAR_S, T.SCHAR, T.UCHAR, T.CHAR_U) or sp.kind in (
                T.VOID, T.CHAR_S, T.SCHAR, T.UCHAR, T.CHAR_U, T.FUNCTIONPROTO):
            return None
        if tp.get_size() <= 0 or sp.get_size() <= 0:
            return None
        if sp.kind in (T.CHAR_S, T.SCHAR, T.UCHAR, T.CHAR_U, T.VOID):
            return None
        return {
            "line": node.location.line, "column": node.location.column, "kind": "pun",
            "from": sp.spelling, "to": tp.spelling, "from_size": sp.get_size(), "to_size": tp.get_size(),
        }


def scan(job):
    path, args, directory = job
    ci = load_clang()
    os.chdir(directory)
    narrow = Arch(ci, path, args, "-m32")
    if narrow.errors:
        return path, [], [str(narrow.errors[0])]
    wide = Arch(ci, path, args, "-m64")
    if wide.errors:
        return path, [], [str(wide.errors[0])]
    n_hits = {(h["line"], h["column"], h["kind"]): h for h in Scanner(narrow).walk()}
    w_hits = {(h["line"], h["column"], h["kind"]): h for h in Scanner(wide).walk()}
    source = Path(path).read_text(errors="replace").splitlines()
    rel = Path(path).relative_to(ROOT) if ROOT in Path(path).parents else Path(path)
    out = []
    for key, h in sorted(n_hits.items()):
        w = w_hits.get(key)
        text = " ".join(source[h["line"] - 1].split())
        where = f"{rel}:{h['line']}:{h['column']}"
        if h["kind"] == "pun":
            if w is None:
                continue
            if h["from_size"] == h["to_size"] and w["from_size"] != w["to_size"]:
                out.append((f"{rel}: {text}", f"{where}: pun {h['from']}* -> {h['to']}* "
                            f"(i386 {h['from_size']}/{h['to_size']}, x86_64 {w['from_size']}/{w['to_size']}): {text}"))
            continue
        if h["kind"] == "width":
            if w is not None and w["to_size"] != h["to_size"]:
                out.append((f"{rel}: {text}", f"{where}: {h['to']} (i386 {h['to_size']} bytes, x86_64 "
                            f"{w['to_size']}) read or written at a byte offset: {text}"))
            continue
        if h["kind"] == "union":
            if w is not None and h["field_size"] == h["union_size"] and w["field_size"] != w["union_size"]:
                out.append((f"{rel}: {text}", f"{where}: union {h['union']} copied through its "
                            f"{h['field_size']}-byte member {h['field']} (x86_64 union is {w['union_size']}): {text}"))
            continue
        if w is None or h["record"] is None:
            continue
        rt32 = narrow.records.get(h["record"])
        rt64 = wide.records.get(h["record"])
        if rt32 is None or rt64 is None:
            continue
        reasons = []
        if h["offset"]:
            byte32 = h["start"] + h["offset"] * h["unit"]
            byte64 = w["start"] + w["offset"] * w["unit"]
            size32 = narrow.size(rt32)
            if byte32 >= size32 or byte32 < 0:
                # past the record: count whole records, then the field inside
                k, rest = divmod(byte32, size32)
                path32 = narrow.path_at(rt32, rest)
                want64 = None
                off = wide.offset_of(rt64, path32)
                if off is not None:
                    want64 = k * wide.size(rt64) + off
            else:
                path32 = narrow.path_at(rt32, byte32)
                want64 = wide.offset_of(rt64, path32)
            if want64 is None or want64 != byte64 or h["unit"] != w["unit"] and h["kind"] != "spill":
                name = narrow.path_name(rt32, path32)
                reasons.append(f"i386 byte {byte32} = {name}; x86_64 reads byte {byte64}, "
                               f"field is at {want64 if want64 is not None else '?'}")
        if h["kind"] == "offset" and h["unit"] != w["unit"] and not reasons:
            reasons.append(f"element size {h['unit']} on i386, {w['unit']} on x86_64")
        if h["strides"]:
            n_sizes = narrow.sizes_inside(rt32)
            w_sizes = wide.sizes_inside(rt64)
            for k in h["strides"]:
                for name, size in n_sizes.items():
                    if size == k * (h["unit"] if h["kind"] == "offset" else 1) and w_sizes.get(name, size) != size:
                        reasons.append(f"stride {k} = i386 sizeof({name}), x86_64 {w_sizes[name]}")
                        break
        if reasons:
            out.append((f"{rel}: {text}", f"{where}: {h['kind']} from {h['record']}: "
                        + "; ".join(reasons) + f": {text}"))
    return path, out, []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, action="append", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--allow", type=Path, default=DEFAULT_ALLOW)
    parser.add_argument("--only", nargs="*", default=None)
    parser.add_argument("--include-raw-loaders", action="store_true")
    parser.add_argument("--changed-since", metavar="COMMIT",
                        help="scan only the units a change since this commit can reach (changed_units.py)")
    args = parser.parse_args()
    load_clang()
    allowed = load_allow(args.allow)
    jobs, seen = [], set()
    for build in args.build_dir:
        for p, a, d in translation_units(build.resolve()):
            if p in seen:
                continue
            seen.add(p)
            if args.only is not None and not any(o in p for o in args.only):
                continue
            if str(Path(p).relative_to(ROOT)) in RETAIL_RECORD_SOURCES:
                continue
            if not args.include_raw_loaders and raw_asset_loader(p):
                continue
            jobs.append((p, a, d))
    if args.changed_since:
        import changed_units
        reach, why = changed_units.select({job[0] for job in jobs}, [build.resolve() for build in args.build_dir], args.changed_since)
        print(f"--changed-since {args.changed_since}: " + (why if reach is not None else f"scanning everything, {why}"))
        if reach is not None:
            jobs = [job for job in jobs if job[0] in reach]
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
        print(f"32-BIT LAYOUT: {text}")
    for line in failures:
        print(f"PARSE ERROR: {line}")
    scanned = {str(Path(p).relative_to(ROOT)) for p, _, _ in jobs}
    stale = sorted(a for a in allowed - used if a.split(":", 1)[0] in scanned)
    for key in stale:
        print(f"STALE ALLOW ENTRY: {key}")
    if failures:
        return 2
    if stale:
        return 1
    if hits:
        print(f"{len(hits)} place(s) use an i386 layout that differs on x86_64")
        return 1
    print(f"no 32-bit layout offsets in {len(jobs)} translation units")
    return 0


if __name__ == "__main__":
    sys.exit(main())
