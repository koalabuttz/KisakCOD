#!/usr/bin/env python3
"""Classify float-to-int conversions in a 32-bit x86 Windows executable.

Hex-Rays prints both a rounding x87 `fistp` and a truncating `_ftol2` call as
`(int)x`, so decompiled source cannot tell which one retail used. This scanner
reads the disassembly instead (upstream KisakCOD issues #47, #52 and #56).

It reports only derived facts: instruction kinds, counts, virtual addresses,
base registers and displacements. It never prints instruction bytes, strings or
data from the executable, so its output is safe to paste into a thread.

Conversion kinds:
  fistp      x87 `fistp`/`fist` with no nearby control-word change. Rounds to
             nearest (ties to even) under the default FPU mode.
  fistp-cw   `fistp` right after an `fldcw`. Normally truncation.
  cvtt       SSE `cvttss2si`/`cvttsd2si`. Truncates.
  cvt        SSE `cvtss2si`/`cvtsd2si`. Rounds under the default MXCSR mode.
  call-ftol  call to a CRT helper whose body truncates (`_ftol2_sse` and
             friends). Truncates.

Usage:
  retail_float_int_scan.py iw3mp.exe
  retail_float_int_scan.py --objdump-text disasm.txt [exe]  # pre-made objdump -d -M intel

With the executable available, float constants used next to each reconverted
site are printed as numbers (e.g. fmul:10), which is how the single sites in
#52 (CutFloat, the flinch yaw, the compass fractions) are told apart.
"""

from __future__ import annotations

import argparse
import collections
import re
import subprocess
import sys
from dataclasses import dataclass, field

LINE_RE = re.compile(r"^\s*([0-9a-f]+):\s+(?:(?:[0-9a-f]{2} )+\s*)?([a-z][a-z0-9.]*)\s*(.*?)\s*$")
MEM_RE = re.compile(r"\[([^\]]+)\]")
CALL_TARGET_RE = re.compile(r"^(?:0x)?([0-9a-f]+)\b")

# Known MP struct offsets (src/qcommon/ent.h, q_shared.h, bg_local.h), used only
# to label displacement triples in the report.
KNOWN_DISPLACEMENTS = {
    0x00: "vec3 at base (Sys_SnapVector shape)",
    0x04: "objective_t.origin",
    0x18: "entityState_s.lerp.pos.trBase",
    0x24: "entityState_s.lerp.pos.trDelta",
    0x3C: "entityState_s.lerp.apos.trBase",
}

X87_LOADS = {"fld"}
X87_INT_STORES = {"fistp", "fist", "fisttp"}
SSE_TRUNC = {"cvttss2si", "cvttsd2si"}
SSE_ROUND = {"cvtss2si", "cvtsd2si"}
RECONVERT = {"fild", "cvtsi2ss", "cvtsi2sd"}
# x87 ops that take the int operand straight from memory (fisub [tmp]).
INT_OPERAND_OPS = {"fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr", "ficom", "ficomp"}
ARITH_NEXT = (
    "fsub", "fsubr", "fsubp", "fsubrp", "fadd", "faddp", "fmul", "fmulp",
    "fdiv", "fdivr", "fdivp", "fdivrp", "fcomp", "fcom", "fucomp", "fucom",
    "fstp", "fst", "subss", "subsd", "mulss", "mulsd", "addss", "addsd",
    "movss", "movsd", "comiss", "ucomiss",
)


@dataclass
class Insn:
    addr: int
    mnem: str
    ops: str


@dataclass
class Site:
    index: int
    addr: int
    kind: str
    src: tuple[str, int] | None = None
    dst: tuple[str, int] | None = None
    reconverted: bool = False
    next_op: str = "-"
    consts: list = field(default_factory=list)


@dataclass
class Report:
    kinds: collections.Counter = field(default_factory=collections.Counter)
    helpers: list = field(default_factory=list)
    sites: list = field(default_factory=list)
    triples: list = field(default_factory=list)
    loose_triples: list = field(default_factory=list)


class PeImage:
    """Minimal PE32 reader: maps virtual addresses to file bytes for constants."""

    def __init__(self, data: bytes):
        import struct
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        self.image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        self.sections = []
        sec = pe + 24 + opt_size
        for n in range(nsec):
            vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, sec + 40 * n + 8)
            self.sections.append((self.image_base + va, max(vsize, raw_size), raw_off, raw_size))

    def read(self, va: int, size: int) -> bytes | None:
        for start, vsize, raw_off, raw_size in self.sections:
            if start <= va and va + size <= start + min(vsize, raw_size):
                off = raw_off + (va - start)
                return self.data[off:off + size]
        return None

    def number(self, va: int, size: int) -> float | None:
        import struct
        raw = self.read(va, size)
        if raw is None or len(raw) != size:
            return None
        return struct.unpack("<f" if size == 4 else "<d", raw)[0]


CONST_RE = re.compile(r"(DWORD|QWORD) PTR [a-z]s:0x([0-9a-f]+)")
CONST_MNEMS = {
    "fld", "fmul", "fadd", "fsub", "fsubr", "fdiv", "fdivr", "fcom", "fcomp",
    "movss", "movsd", "mulss", "mulsd", "addss", "addsd", "subss", "subsd", "comiss", "ucomiss",
}


def nearby_constants(insns, i, pe, window=10):
    """Float constants loaded from absolute addresses around insns[i]."""
    if pe is None:
        return []
    out = []
    for j in range(max(0, i - window), min(len(insns), i + window + 1)):
        insn = insns[j]
        if insn.mnem not in CONST_MNEMS:
            continue
        m = CONST_RE.search(insn.ops)
        if not m:
            continue
        value = pe.number(int(m.group(2), 16), 4 if m.group(1) == "DWORD" else 8)
        if value is not None and value == value and abs(value) < 1e9:
            out.append(f"{insn.mnem}:{value:.9g}")
    return out


def parse_objdump(text: str) -> list[Insn]:
    insns = []
    for line in text.splitlines():
        m = LINE_RE.match(line)
        if not m:
            continue
        addr, mnem, ops = m.groups()
        if mnem in ("bad", "(bad)"):
            continue
        insns.append(Insn(int(addr, 16), mnem, ops))
    return insns


def parse_mem(ops: str) -> tuple[str, int] | None:
    """Return (base expression, displacement) for the first memory operand."""
    m = MEM_RE.search(ops)
    if not m:
        absolute = re.search(r"PTR [a-z]s:(0x[0-9a-f]+)", ops)
        return ("abs", int(absolute.group(1), 16)) if absolute else None
    expr = m.group(1).replace(" ", "")
    disp = 0
    base_parts = []
    for term in re.findall(r"[+-]?[^+-]+", expr):
        sign = -1 if term.startswith("-") else 1
        body = term.lstrip("+-")
        if re.fullmatch(r"0x[0-9a-f]+|[0-9]+", body):
            disp += sign * int(body, 0)
        else:
            base_parts.append(body)
    return ("+".join(base_parts) or "abs", disp)


def call_target(ops: str) -> int | None:
    m = CALL_TARGET_RE.match(ops.strip())
    return int(m.group(1), 16) if m else None


def body_from(insns, by_addr, start, limit=48):
    i = by_addr.get(start)
    if i is None:
        return []
    out = []
    for insn in insns[i:i + limit]:
        out.append(insn)
        if insn.mnem.startswith("ret"):
            break
    return out


def find_truncating_helpers(insns, by_addr):
    """Call targets whose body truncates, plus the fallbacks they jump to."""
    calls = collections.Counter()
    for insn in insns:
        if insn.mnem == "call":
            t = call_target(insn.ops)
            if t is not None and t in by_addr:
                calls[t] += 1
    helpers = {}
    for target, count in calls.items():
        body = body_from(insns, by_addr, target)
        mnems = [b.mnem for b in body]
        if any(m in SSE_TRUNC for m in mnems[:16]):
            helpers[target] = ("cvtt", count)
            # _ftol2_sse jumps to the x87 _ftol2 when SSE2 is missing.
            for b in body[:6]:
                if b.mnem.startswith("j"):
                    fb = call_target(b.ops)
                    if fb is not None and fb in by_addr and fb not in helpers:
                        helpers[fb] = ("fallback", calls.get(fb, 0))
        elif "fldcw" in mnems and any(m in X87_INT_STORES for m in mnems):
            helpers[target] = ("fldcw", count)
    # An x87 fallback reached only by jump still counts as helper code.
    ranges = []
    for target in helpers:
        body = body_from(insns, by_addr, target)
        if body:
            ranges.append((body[0].addr, body[-1].addr))
    return helpers, ranges


def in_ranges(addr, ranges):
    return any(lo <= addr <= hi for lo, hi in ranges)


def trace_source(insns, i, window=8):
    """Find the memory a conversion at insns[i] read, looking through spills."""
    insn = insns[i]
    if insn.mnem in SSE_TRUNC | SSE_ROUND:
        mem = parse_mem(insn.ops)
        if mem:
            return mem
        reg = insn.ops.split(",")[-1].strip()
        for j in range(i - 1, max(-1, i - window), -1):
            prev = insns[j]
            if prev.mnem in ("movss", "movsd") and prev.ops.startswith(reg + ","):
                return parse_mem(prev.ops)
        return None
    src = None
    for j in range(i - 1, max(-1, i - window), -1):
        prev = insns[j]
        if prev.mnem in X87_INT_STORES or prev.mnem == "call":
            break
        if prev.mnem in X87_LOADS:
            src = parse_mem(prev.ops)
            if src is None:
                return None
            # fld [x]; fstp [tmp]; fld [tmp]; fistp -> follow back to x.
            for k in range(j - 1, max(-1, j - window), -1):
                spill = insns[k]
                if spill.mnem == "fstp" and parse_mem(spill.ops) == src:
                    for m in range(k - 1, max(-1, k - window), -1):
                        if insns[m].mnem in X87_LOADS:
                            return parse_mem(insns[m].ops) or src
                    break
            return src
    return src


def follow_result(insns, i, window=8):
    """Did the int result get turned back into a float, and what came next?"""
    reconverted = False
    for j in range(i + 1, min(len(insns), i + 1 + window)):
        insn = insns[j]
        if insn.mnem in X87_INT_STORES or insn.mnem in SSE_TRUNC | SSE_ROUND:
            break
        if insn.mnem == "call":
            break
        if insn.mnem in INT_OPERAND_OPS:
            return True, insn.mnem, None
        if not reconverted and insn.mnem in RECONVERT:
            reconverted = True
            continue
        if reconverted and insn.mnem in ARITH_NEXT:
            dst = parse_mem(insn.ops) if insn.mnem in ("fstp", "fst", "movss") else None
            if insn.mnem in ("movss", "movsd") and not insn.ops.startswith(("DWORD", "QWORD")):
                continue
            return True, insn.mnem, dst
    return reconverted, "-", None


def scan(insns: list[Insn], pe: PeImage | None = None) -> Report:
    insns.sort(key=lambda x: x.addr)
    by_addr = {insn.addr: i for i, insn in enumerate(insns)}
    helpers, helper_ranges = find_truncating_helpers(insns, by_addr)
    report = Report()
    report.helpers = sorted((addr, kind, count) for addr, (kind, count) in helpers.items())

    for i, insn in enumerate(insns):
        kind = None
        if insn.mnem in X87_INT_STORES:
            if in_ranges(insn.addr, helper_ranges):
                continue
            recent = [insns[j].mnem for j in range(max(0, i - 6), i)]
            kind = "fistp-cw" if "fldcw" in recent else "fistp"
            if insn.mnem == "fisttp":
                kind = "fisttp"
        elif insn.mnem in SSE_TRUNC:
            if in_ranges(insn.addr, helper_ranges):
                continue
            kind = "cvtt"
        elif insn.mnem in SSE_ROUND:
            kind = "cvt"
        elif insn.mnem == "call":
            t = call_target(insn.ops)
            if t in helpers:
                kind = "call-ftol"
        if kind is None:
            continue
        site = Site(index=i, addr=insn.addr, kind=kind)
        site.src = trace_source(insns, i)
        site.reconverted, site.next_op, site.dst = follow_result(insns, i)
        site.consts = nearby_constants(insns, i, pe)
        report.kinds[kind] += 1
        report.sites.append(site)

    # Snap triples: three reconverted conversions reading base+d, +4, +8.
    snaps = [s for s in report.sites if s.reconverted and s.src]
    used = set()
    for a_i, a in enumerate(snaps):
        if a.addr in used:
            continue
        found = [a]
        for b in snaps[a_i + 1:a_i + 8]:
            if b.addr - a.addr > 0x200:
                break
            want = (a.src[0], a.src[1] + 4 * len(found))
            if b.src == want:
                found.append(b)
                if len(found) == 3:
                    break
        if len(found) == 3:
            used.update(s.addr for s in found)
            report.triples.append(found)

    # Loose triples: three nearby conversions whose source or float
    # destination steps by 4 bytes (catches snaps copied to a local, or
    # reached through an index register), whether or not they are reconverted.
    strict = {s.addr for t in report.triples for s in t}
    sites = report.sites
    for a_i, a in enumerate(sites):
        if a.addr in strict:
            continue
        for key in ("src", "dst"):
            ref = getattr(a, key)
            if ref is None:
                continue
            found = [a]
            for b in sites[a_i + 1:a_i + 8]:
                if b.addr - a.addr > 0x180:
                    break
                if getattr(b, key) == (ref[0], ref[1] + 4 * len(found)):
                    found.append(b)
                    if len(found) == 3:
                        break
            if len(found) == 3 and not any(s.addr in strict for s in found):
                report.loose_triples.append((key, found))
                break
    return report


def format_report(report: Report, max_sites: int) -> str:
    out = []
    out.append("float-to-int conversion kinds (outside CRT helpers):")
    for kind, count in sorted(report.kinds.items()):
        out.append(f"  {kind:10s} {count}")
    out.append("")
    out.append("truncating CRT helpers (address, how detected, call count):")
    for addr, kind, count in report.helpers:
        out.append(f"  0x{addr:08x} {kind:8s} {count}")
    out.append("")

    out.append("snap triples (int result stored back as float, base+d, +4, +8):")
    for triple in report.triples:
        base, disp = triple[0].src
        kinds = "/".join(s.kind for s in triple)
        label = KNOWN_DISPLACEMENTS.get(disp, "")
        dst = triple[0].dst
        same = "in place" if dst == triple[0].src else (f"to [{dst[0]}+0x{dst[1]:x}]" if dst else "to ?")
        out.append(f"  0x{triple[0].addr:08x} {kinds:30s} [{base}+0x{disp:x}] {same} {label}")
    out.append(f"  ({len(report.triples)} triples)")
    out.append("")

    out.append("loose triples (source or destination steps by 4; any kind):")
    for key, triple in report.loose_triples:
        ref = getattr(triple[0], key)
        kinds = "/".join(s.kind for s in triple)
        backs = "/".join(s.next_op if s.reconverted else "int" for s in triple)
        label = KNOWN_DISPLACEMENTS.get(ref[1], "") if key == "src" else ""
        out.append(f"  0x{triple[0].addr:08x} {kinds:30s} {key} [{ref[0]}+0x{ref[1]:x}] then {backs} {label}")
    out.append(f"  ({len(report.loose_triples)} loose triples)")
    out.append("")

    # Everything that converts back to float and then does arithmetic: the
    # fraction, yaw and CutFloat shapes from #52.
    shapes = collections.Counter()
    for s in report.sites:
        if s.reconverted:
            shapes[(s.kind, s.next_op)] += 1
    out.append("reconverted sites by kind and next float op:")
    for (kind, op), count in sorted(shapes.items()):
        out.append(f"  {kind:10s} then {op:8s} {count}")
    out.append("")

    out.append("reconverted sites with nearby float constants (all kinds):")
    for s in report.sites:
        if s.reconverted and s.consts:
            out.append(f"  0x{s.addr:08x} {s.kind:9s} then {s.next_op:6s} {' '.join(s.consts)}")
    out.append("")

    rounding = [s for s in report.sites if s.kind in ("fistp", "cvt")]
    out.append(f"rounding sites ({len(rounding)}; first {max_sites} listed):")
    for s in rounding[:max_sites]:
        src = f"[{s.src[0]}+0x{s.src[1]:x}]" if s.src else "?"
        back = f"reconverted, then {s.next_op}" if s.reconverted else "stays int"
        out.append(f"  0x{s.addr:08x} {s.kind:6s} src {src:24s} {back}")
    return "\n".join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", nargs="?", help="PE32 executable to disassemble with objdump")
    ap.add_argument("--objdump-text", help="read existing `objdump -d -M intel` output instead")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--max-sites", type=int, default=400)
    args = ap.parse_args(argv)
    pe = None
    if args.exe:
        with open(args.exe, "rb") as f:
            pe = PeImage(f.read())
    if args.objdump_text:
        with open(args.objdump_text, encoding="utf-8", errors="replace") as f:
            text = f.read()
    elif args.exe:
        text = subprocess.run(
            [args.objdump, "-d", "-M", "intel", "--no-show-raw-insn", "-w", "-j", ".text", args.exe],
            check=True, capture_output=True, text=True,
        ).stdout
    else:
        ap.error("give an executable or --objdump-text")
    print(format_report(scan(parse_objdump(text), pe), args.max_sites))
    return 0


if __name__ == "__main__":
    sys.exit(main())
