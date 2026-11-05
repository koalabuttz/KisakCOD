#!/usr/bin/env python3
"""A small assembler for Direct3D 9 shader model 2 and 3 bytecode.

Written for the shader translator spike (renderer plan S0): it turns short
test shaders in D3D assembly into the token format the game's shaders use, so
translators can be checked op by op without game files. It covers the
instructions and registers vs_2_0/vs_3_0/ps_2_0/ps_2_x/ps_3_0 use; it is not a
full replacement for fxc's assembler.

Syntax follows the D3D documentation:

    ps_3_0
    dcl_2d s0
    dcl_texcoord0 v0.xy
    def c10, 0, 1, 0.5, 2
    texld r0, v0, s0
    setp_gt p0, r0.x, c10.y
    (p0) mov r0, c1
    mov_sat oC0, |r0|

Every shader gets a constant table (CTAB comment) that declares c0-c<n> as
one float4 array, each used sampler, and any i#/b# registers, the way fxc
output carries one. Pass ``ctab=False`` to leave it out.
"""

from __future__ import annotations

import re
import struct
import sys

OPCODES = {
    "nop": 0, "mov": 1, "add": 2, "sub": 3, "mad": 4, "mul": 5, "rcp": 6,
    "rsq": 7, "dp3": 8, "dp4": 9, "min": 10, "max": 11, "slt": 12, "sge": 13,
    "exp": 14, "log": 15, "lit": 16, "dst": 17, "lrp": 18, "frc": 19,
    "m4x4": 20, "m4x3": 21, "m3x4": 22, "m3x3": 23, "m3x2": 24, "call": 25,
    "callnz": 26, "loop": 27, "ret": 28, "endloop": 29, "label": 30,
    "dcl": 31, "pow": 32, "crs": 33, "sgn": 34, "abs": 35, "nrm": 36,
    "sincos": 37, "rep": 38, "endrep": 39, "if": 40, "ifc": 41, "else": 42,
    "endif": 43, "break": 44, "breakc": 45, "mova": 46, "defb": 47,
    "defi": 48, "texkill": 65, "texld": 66, "expp": 78, "logp": 79,
    "cnd": 80, "def": 81, "cmp": 88, "dp2add": 90, "dsx": 91, "dsy": 92,
    "texldd": 93, "setp": 94, "texldl": 95, "breakp": 96,
}
NO_DST = {"nop", "call", "callnz", "loop", "ret", "endloop", "label", "rep",
          "endrep", "if", "ifc", "else", "endif", "break", "breakc", "breakp"}
# texkill's single operand is encoded as a destination.
COMPARISONS = {"gt": 1, "eq": 2, "ge": 3, "lt": 4, "ne": 5, "le": 6}
USAGES = {"position": 0, "blendweight": 1, "blendindices": 2, "normal": 3,
          "psize": 4, "texcoord": 5, "tangent": 6, "binormal": 7,
          "tessfactor": 8, "positiont": 9, "color": 10, "fog": 11,
          "depth": 12, "sample": 13}
SAMPLER_DCL = {"2d": 2, "cube": 3, "volume": 4}

R_TEMP, R_INPUT, R_CONST, R_ADDR, R_RASTOUT, R_ATTROUT, R_OUTPUT = 0, 1, 2, 3, 4, 5, 6
R_CONSTINT, R_COLOROUT, R_DEPTHOUT, R_SAMPLER = 7, 8, 9, 10
R_CONSTBOOL, R_LOOP, R_MISCTYPE, R_LABEL, R_PREDICATE = 14, 15, 17, 18, 19

SRC_MOD = {"neg": 1, "bias": 2, "bias_neg": 3, "bx2": 4, "bx2_neg": 5,
           "comp": 6, "x2": 7, "x2_neg": 8, "abs": 11, "abs_neg": 12, "not": 13}


class AsmError(ValueError):
    pass


def _reg_bits(kind: int, num: int) -> int:
    if not 0 <= num < 2048:
        raise AsmError("register number %d out of range" % num)
    return 0x80000000 | num | ((kind & 7) << 28) | ((kind & 0x18) << 8)


def _swizzle(text: str) -> int:
    if not text:
        return 0xE4
    idx = ["xyzw".index(c) if c in "xyzw" else "rgba".index(c) for c in text]
    while len(idx) < 4:
        idx.append(idx[-1])
    return idx[0] | (idx[1] << 2) | (idx[2] << 4) | (idx[3] << 6)


def _writemask(text: str) -> int:
    if not text:
        return 0xF
    mask = 0
    for c in text:
        mask |= 1 << ("xyzw".index(c) if c in "xyzw" else "rgba".index(c))
    return mask


class Shader:
    def __init__(self, stage: str, major: int, minor: int):
        self.stage, self.major, self.minor = stage, major, minor
        self.tokens = [(0xFFFE0000 if stage == "vs" else 0xFFFF0000) | (major << 8) | minor]
        self.max_const = -1
        self.samplers = {}
        self.int_regs, self.bool_regs = set(), set()
        self.defined = set()

    # registers -------------------------------------------------------------
    def register(self, name: str):
        """Return (kind, number) for a register name such as r3, oC0, vPos."""
        n = name.lower()
        fixed = {"vpos": (R_MISCTYPE, 0), "vface": (R_MISCTYPE, 1),
                 "odepth": (R_DEPTHOUT, 0), "opos": (R_RASTOUT, 0),
                 "ofog": (R_RASTOUT, 1), "opts": (R_RASTOUT, 2),
                 "al": (R_LOOP, 0), "a0": (R_ADDR, 0), "p0": (R_PREDICATE, 0)}
        if n in fixed:
            return fixed[n]
        m = re.fullmatch(r"(oc|od|ot|r|v|c|i|b|s|o|t|l)(\d+)", n)
        if not m:
            raise AsmError("unknown register %r" % name)
        prefix, num = m.group(1), int(m.group(2))
        kind = {"r": R_TEMP, "v": R_INPUT, "c": R_CONST, "i": R_CONSTINT,
                "b": R_CONSTBOOL, "s": R_SAMPLER, "oc": R_COLOROUT,
                "od": R_ATTROUT, "l": R_LABEL}.get(prefix)
        if prefix == "o":
            kind = R_OUTPUT  # vs_3_0 o#
        elif prefix == "ot":
            kind = R_OUTPUT  # vs_2_0 oT# shares the output register type
        elif prefix == "t":
            kind = R_ADDR    # ps_2_0 texture coordinate input shares type 3
        if kind == R_CONST:
            self.max_const = max(self.max_const, num)
        elif kind == R_CONSTINT:
            self.int_regs.add(num)
        elif kind == R_CONSTBOOL:
            self.bool_regs.add(num)
        return kind, num

    def dst(self, text: str, mods=()):
        m = re.fullmatch(r"([A-Za-z]+\d*)(?:\[(aL)\s*(?:\+\s*(\d+))?\])?(?:\.([xyzwrgba]+))?", text.strip())
        if not m:
            raise AsmError("bad destination %r" % text)
        base, rel_reg, rel_off, mask = m.groups()
        if rel_reg:
            base = base + str(int(rel_off or 0))
        kind, num = self.register(base)
        token = _reg_bits(kind, num) | (_writemask(mask or "") << 16)
        for mod in mods:
            token |= {"sat": 1, "pp": 2, "centroid": 4}[mod] << 20
        if not rel_reg:
            return [token]
        rkind, rnum = self.register(rel_reg)
        return [token | (1 << 13), _reg_bits(rkind, rnum) | (0xE4 << 16)]

    def src(self, text: str):
        t = text.strip()
        mod = None
        if t.startswith("-"):
            mod, t = "neg", t[1:].strip()
        if t.startswith("|") and t.endswith("|"):
            mod = "abs_neg" if mod == "neg" else "abs"
            t = t[1:-1]
        elif t.startswith("!"):
            mod, t = "not", t[1:]
        m = re.fullmatch(r"([A-Za-z]+\d*)(?:\[(a0|aL)(?:\.([xyzw]))?\s*(?:\+\s*(\d+))?\])?(?:_(bx2|bias|x2|comp))?(?:\.([xyzwrgba]+))?", t)
        if not m:
            raise AsmError("bad source %r" % text)
        base, rel_reg, rel_comp, rel_off, suffix, swz = m.groups()
        if rel_reg and base.lower() == "c":
            base = "c%d" % int(rel_off or 0)
        elif rel_reg and re.fullmatch(r"[a-z]+", base.lower()):
            base = base + str(int(rel_off or 0))
        kind, num = self.register(base)
        if suffix:
            mod = suffix + ("_neg" if mod == "neg" else "")
        token = _reg_bits(kind, num) | (_swizzle(swz or "") << 16)
        if mod:
            token |= SRC_MOD[mod] << 24
        out = [token]
        if rel_reg:
            token |= 1 << 13
            out[0] = token
            rkind, rnum = self.register(rel_reg)
            comp = "xyzw".index(rel_comp or "x")
            out.append(_reg_bits(rkind, rnum) | ((comp * 0x55) << 16))
        return out

    # instructions ----------------------------------------------------------
    def emit(self, opcode: int, params, control: int = 0, predicated: bool = False):
        token = opcode | (control << 16) | (len(params) << 24)
        if predicated:
            token |= 1 << 28
        self.tokens.append(token)
        self.tokens.extend(params)

    def comment(self, payload: bytes):
        payload += b"\0" * (-len(payload) % 4)
        words = struct.unpack("<%dI" % (len(payload) // 4), payload)
        self.tokens.append(0xFFFE | (len(words) << 16))
        self.tokens.extend(words)

    def line(self, text: str):
        text = text.split("//")[0].strip()
        if not text:
            return
        predicated = False
        pred_tokens = []
        m = re.match(r"\((!?)(p0(?:\.[xyzw]+)?)\)\s*(.*)", text)
        if m:
            predicated = True
            pred_tokens = self.src(("!" if m.group(1) else "") + m.group(2))
            text = m.group(3)
        op, _, rest = text.partition(" ")
        args = [a.strip() for a in rest.split(",")] if rest.strip() else []
        parts = op.lower().split("_")
        name, mods = parts[0], parts[1:]

        if name == "dcl":
            self._dcl(mods, args)
            return
        if name in ("def", "defi", "defb"):
            self._def(name, args)
            return
        control = 0
        dst_mods = []
        for mod in mods:
            if mod in COMPARISONS:
                control = COMPARISONS[mod]
            elif mod in ("sat", "pp", "centroid"):
                dst_mods.append(mod)
            else:
                raise AsmError("unknown modifier _%s" % mod)
        if name == "texldp":
            name, control = "texld", 1
        elif name == "texldb":
            name, control = "texld", 2
        if name not in OPCODES:
            raise AsmError("unknown instruction %r" % op)
        params = []
        if name == "label":
            # The label operand carries no swizzle; encode it like a full-mask destination.
            params += self.dst(args[0])
        elif name in NO_DST:
            for a in args:
                params += self.src(a)
        else:
            params += self.dst(args[0], dst_mods)
            params += pred_tokens
            pred_tokens = []
            for a in args[1:]:
                params += self.src(a)
        params += pred_tokens
        self.emit(OPCODES[name], params, control, predicated)

    def _dcl(self, mods, args):
        reg = args[0]
        kind, num = self.register(re.split(r"\.", reg)[0])
        if kind == R_SAMPLER:
            sampler_type = mods[0] if mods else "2d"
            self.samplers[num] = sampler_type
            usage_token = 0x80000000 | (SAMPLER_DCL[sampler_type] << 27)
            self.emit(OPCODES["dcl"], [usage_token, self.dst(reg)[0]])
            return
        dst_mods = [m for m in mods if m in ("centroid", "pp")]
        usage = [m for m in mods if m not in ("centroid", "pp")]
        token = 0x80000000
        if usage:
            um = re.fullmatch(r"([a-z]+?)(\d*)", usage[0])
            token |= USAGES[um.group(1)] | (int(um.group(2) or 0) << 16)
        self.emit(OPCODES["dcl"], [token, self.dst(reg, dst_mods)[0]])

    def _def(self, name, args):
        dst = self.dst(args[0])[0]
        kind, num = self.register(args[0])
        self.defined.add((kind, num))
        if name == "def":
            vals = [struct.unpack("<I", struct.pack("<f", float(a)))[0] for a in args[1:5]]
        elif name == "defi":
            vals = [int(a) & 0xFFFFFFFF for a in args[1:5]]
        else:
            vals = [1 if args[1].lower() in ("true", "1") else 0]
        self.emit(OPCODES[name], [dst] + vals)

    # constant table --------------------------------------------------------
    def ctab(self) -> bytes:
        """Build a CTAB the way fxc lays it out (header, constants, types, names)."""
        entries = []
        if self.max_const >= 0:
            entries.append(("C", 2, 0, self.max_const + 1, (1, 3, 1, 4, self.max_const + 1)))
        for num in sorted(self.int_regs):
            entries.append(("I%d" % num, 1, num, 1, (1, 2, 1, 4, 1)))
        for num in sorted(self.bool_regs):
            entries.append(("B%d" % num, 0, num, 1, (0, 1, 1, 1, 1)))
        for num, kind in sorted(self.samplers.items()):
            stype = {"2d": 12, "cube": 14, "volume": 13}[kind]
            entries.append(("S%d" % num, 3, num, 1, (4, stype, 1, 1, 1)))
        header_size = 28
        info_size = 20 * len(entries)
        type_off = header_size + info_size
        blob = bytearray()
        types = bytearray()
        names = bytearray()
        strings_off = type_off + 16 * len(entries)
        target = ("%s_%d_%d" % (self.stage, self.major, self.minor)).encode() + b"\0"
        creator = b"kisak d3d9_asm\0"
        creator_off = strings_off
        target_off = creator_off + len(creator)
        name_off = target_off + len(target)
        infos = bytearray()
        for i, (name, regset, index, count, tinfo) in enumerate(entries):
            infos += struct.pack("<IHHHHII", name_off + len(names), regset, index, count, 0,
                                 type_off + 16 * i, 0)
            names += name.encode() + b"\0"
            cls, typ, rows, cols, elems = tinfo
            types += struct.pack("<HHHHHHI", cls, typ, rows, cols, elems, 0, 0)
        version = self.tokens[0]
        blob += struct.pack("<7I", header_size, creator_off, version, len(entries),
                            header_size if entries else 0, 0, target_off)
        blob += infos + types + creator + target + names
        return b"CTAB" + bytes(blob)


def assemble(source: str, ctab: bool = True) -> bytes:
    lines = [l for l in source.splitlines() if l.split("//")[0].strip()]
    if not lines:
        raise AsmError("empty shader")
    m = re.fullmatch(r"(vs|ps)_(\d)_(\d|x)", lines[0].strip().lower())
    if not m:
        raise AsmError("first line must be a version such as ps_3_0")
    minor = 1 if m.group(3) == "x" else int(m.group(3))
    shader = Shader(m.group(1), int(m.group(2)), minor)
    for number, text in enumerate(lines[1:], start=2):
        try:
            shader.line(text)
        except (AsmError, ValueError, KeyError, IndexError) as exc:
            raise AsmError("line %d (%s): %s" % (number, text.strip(), exc)) from None
    body = shader.tokens[1:]
    shader.tokens = shader.tokens[:1]
    if ctab:
        shader.comment(shader.ctab())
    shader.tokens += body
    shader.tokens.append(0x0000FFFF)
    return struct.pack("<%dI" % len(shader.tokens), *shader.tokens)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print("usage: d3d9_asm.py <in.asm> <out.bin>", file=sys.stderr)
        return 2
    with open(argv[0]) as f:
        code = assemble(f.read())
    with open(argv[1], "wb") as f:
        f.write(code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
