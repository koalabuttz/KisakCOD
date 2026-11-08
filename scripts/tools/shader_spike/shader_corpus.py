#!/usr/bin/env python3
"""Count the D3D9 shaders in CoD4 fastfiles and the bytecode features they use.

This is the corpus step of the renderer plan's shader spike (S0). It reads the
zone files of a local game install, finds every vertex and pixel shader the
zones carry, and reports derived numbers only: counts per shader model, per
renderer path, per family, per opcode and per D3D9 feature, plus which
features each shader translator candidate is predicted to reject.

The game's bytecode never goes into the report. ``--dump-dir`` can write the
unique programs to a local folder for translator runs on the same machine; the
folder must be outside this repository.

Finding shaders: a fastfile is "IWffu100", a version dword (5) and a zlib
stream holding the zone in load order. An inline MaterialVertexShader or
MaterialPixelShader is written as its 16-byte struct (name pointer -1,
program handle, program pointer -1, programSize in dwords, loadForRenderer),
then the NUL-terminated name, then programSize dwords of bytecode (see
Load_MaterialPixelShader in src/database/db_load.cpp). Shaders referenced by
offset were written inline earlier in the same zone, so they are not lost.

Usage:
  shader_corpus.py --group mp=<zone>/mp_*.ff --group sp=<zone>/*.ff \
      --out report.json [--dump-dir /tmp/cod4-shaders]

When a file matches several groups, the first group that lists it wins.
"""

from __future__ import annotations

import argparse
import collections
import fnmatch
import glob
import hashlib
import json
import os
import struct
import sys
import zlib

FASTFILE_MAGICS = (b"IWffu100", b"IWff0100")
FASTFILE_VERSION = 5

# D3DSIO opcodes, shader models 1-3.
OPCODES = {
    0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp",
    7: "rsq", 8: "dp3", 9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge",
    14: "exp", 15: "log", 16: "lit", 17: "dst", 18: "lrp", 19: "frc",
    20: "m4x4", 21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2", 25: "call",
    26: "callnz", 27: "loop", 28: "ret", 29: "endloop", 30: "label",
    31: "dcl", 32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm",
    37: "sincos", 38: "rep", 39: "endrep", 40: "if", 41: "ifc", 42: "else",
    43: "endif", 44: "break", 45: "breakc", 46: "mova", 47: "defb",
    48: "defi", 64: "texcoord", 65: "texkill", 66: "texld", 67: "texbem",
    68: "texbeml", 69: "texreg2ar", 70: "texreg2gb", 71: "texm3x2pad",
    72: "texm3x2tex", 73: "texm3x3pad", 74: "texm3x3tex", 76: "texm3x3spec",
    77: "texm3x3vspec", 78: "expp", 79: "logp", 80: "cnd", 81: "def",
    82: "texreg2rgb", 83: "texdp3tex", 84: "texm3x2depth", 85: "texdp3",
    86: "texm3x3", 87: "texdepth", 88: "cmp", 89: "bem", 90: "dp2add",
    91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texldl",
    96: "breakp", 0xFFFD: "phase",
}
OP_COMMENT = 0xFFFE
OP_END = 0xFFFF

# Opcodes whose operands are all sources (no destination token).
NO_DST = {"nop", "call", "callnz", "loop", "ret", "endloop", "label", "rep",
          "endrep", "if", "ifc", "else", "endif", "break", "breakc", "breakp",
          "phase"}

# D3DSPR register types.
REG_TEMP, REG_INPUT, REG_CONST, REG_ADDR_TEXTURE = 0, 1, 2, 3
REG_RASTOUT, REG_ATTROUT, REG_OUTPUT, REG_CONSTINT = 4, 5, 6, 7
REG_COLOROUT, REG_DEPTHOUT, REG_SAMPLER = 8, 9, 10
REG_CONST2, REG_CONST3, REG_CONST4, REG_CONSTBOOL = 11, 12, 13, 14
REG_LOOP, REG_TEMPFLOAT16, REG_MISCTYPE, REG_LABEL, REG_PREDICATE = 15, 16, 17, 18, 19

SAMPLER_TYPES = {2: "2d", 3: "cube", 4: "volume"}
SRC_MODS = {1: "neg", 2: "bias", 3: "bias_neg", 4: "sign", 5: "sign_neg",
            6: "comp", 7: "x2", 8: "x2_neg", 9: "dz", 10: "dw", 11: "abs",
            12: "abs_neg", 13: "not"}

# Shader families, matched against the shader name in order. These are our
# labels; a name that matches nothing is counted as "other".
FAMILY_RULES = (
    ("cinematic", ("cinematic*",)),
    ("ui_font", ("*font*", "*2d*", "ui_*")),
    ("postfx", ("postfx*", "*glow*", "*bloom*", "*film*", "*blur*", "*dof*",
                "*colormatrix*", "*frame*", "*resolve*")),
    ("shadow_build", ("*shadowmap*build*", "*build_shadowmap*", "*shadowcookie*",
                      "*shadowmap*")),
    ("depth_prepass", ("*depth*", "*floatz*", "*float_z*")),
    ("sky", ("*sky*",)),
    ("water", ("*water*",)),
    ("effects", ("*particle*", "*effect*", "*fx*", "*spark*", "*smoke*",
                 "*distort*")),
    ("decal", ("*decal*",)),
    ("world_lit", ("lm_*", "*lightmap*", "l_*", "*_lit*", "*sun*", "*spot*",
                   "*omni*")),
    ("unlit", ("*unlit*", "*emissive*", "*tools*", "*debug*", "*wireframe*")),
)


class BytecodeError(ValueError):
    pass


def classify_family(name: str) -> str:
    lowered = name.lower()
    for family, patterns in FAMILY_RULES:
        if any(fnmatch.fnmatchcase(lowered, p) for p in patterns):
            return family
    return "other"


def name_prefix(name: str) -> str:
    base = name.lower().split(".")[0]
    for sep in ("_", "-"):
        base = base.split(sep)[0]
    return base or "(empty)"


def _reg_type(token: int) -> int:
    return ((token >> 28) & 0x7) | ((token >> 8) & 0x18)


def parse_bytecode(code: bytes) -> dict:
    """Walk an SM2/SM3 token stream and return its feature summary."""
    if len(code) % 4 or len(code) < 8:
        raise BytecodeError("size is not a whole number of dwords")
    tokens = struct.unpack("<%dI" % (len(code) // 4), code)
    version = tokens[0]
    kind = version >> 16
    if kind == 0xFFFE:
        stage = "vs"
    elif kind == 0xFFFF:
        stage = "ps"
    else:
        raise BytecodeError("bad version token %#x" % version)
    major, minor = (version >> 8) & 0xFF, version & 0xFF
    if major < 2:
        raise BytecodeError("shader model %d.%d is not parsed" % (major, minor))

    ops = collections.Counter()
    features = set()
    sampler_types = {}
    max_const = -1
    inputs, outputs = set(), set()
    comment_dwords = 0
    pos = 1
    ended = False
    while pos < len(tokens):
        token = tokens[pos]
        opcode = token & 0xFFFF
        if opcode == OP_END:
            ended = True
            pos += 1
            break
        if opcode == OP_COMMENT:
            length = (token >> 16) & 0x7FFF
            comment_dwords += length
            if length >= 1 and pos + 1 < len(tokens) and tokens[pos + 1] == 0x42415443:
                features.add("ctab")
            pos += 1 + length
            continue
        length = (token >> 24) & 0xF
        name = OPCODES.get(opcode)
        if name is None:
            raise BytecodeError("unknown opcode %#x at dword %d" % (opcode, pos))
        if pos + 1 + length > len(tokens):
            raise BytecodeError("instruction runs past the end")
        params = tokens[pos + 1: pos + 1 + length]
        control = (token >> 16) & 0xFF
        ops[name] += 1
        if token & (1 << 28):
            features.add("predicated_instruction")
        if name == "texld":
            if stage == "ps" and control & 1:
                ops["texldp"] += 1
                ops["texld"] -= 1
            elif stage == "ps" and control & 2:
                ops["texldb"] += 1
                ops["texld"] -= 1

        if name in ("def", "defi", "defb"):
            reg = params[0]
            if _reg_type(reg) == REG_CONST:
                max_const = max(max_const, reg & 0x7FF)
                features.add("def_float")
            else:
                features.add(name + "_used")
        elif name == "dcl":
            usage, reg = params[0], params[1]
            reg_type, reg_num = _reg_type(reg), reg & 0x7FF
            if reg_type == REG_SAMPLER:
                sampler_types[reg_num] = SAMPLER_TYPES.get((usage >> 27) & 0xF, "unknown")
                if stage == "vs":
                    features.add("vertex_texture_fetch")
            else:
                semantic = (usage & 0x1F, (usage >> 16) & 0xF)
                if reg_type == REG_MISCTYPE:
                    features.add("vpos" if reg_num == 0 else "vface")
                elif reg_type == REG_INPUT or (stage == "ps" and reg_type == REG_ADDR_TEXTURE):
                    inputs.add((reg_type, reg_num) + semantic)
                elif reg_type == REG_OUTPUT:
                    outputs.add((reg_num,) + semantic)
                if (reg >> 20) & 4:
                    features.add("centroid")
        else:
            has_dst = name not in NO_DST
            i = 0
            index = 0
            while i < len(params):
                reg = params[i]
                reg_type, reg_num = _reg_type(reg), reg & 0x7FF
                is_dst = has_dst and index == 0
                relative = bool(reg & (1 << 13))
                if reg_type == REG_CONST:
                    max_const = max(max_const, reg_num)
                elif reg_type == REG_CONSTINT:
                    features.add("int_constants")
                elif reg_type == REG_CONSTBOOL:
                    features.add("bool_constants")
                elif reg_type == REG_MISCTYPE:
                    features.add("vpos" if reg_num == 0 else "vface")
                elif reg_type == REG_DEPTHOUT and is_dst:
                    features.add("depth_output")
                elif reg_type == REG_COLOROUT and is_dst and reg_num > 0:
                    features.add("mrt")
                elif reg_type == REG_LOOP and not is_dst:
                    features.add("loop_counter_operand")
                if is_dst:
                    mod = (reg >> 20) & 0xF
                    if mod & 1:
                        features.add("saturate")
                    if mod & 2:
                        features.add("partial_precision")
                    if mod & 4:
                        features.add("centroid")
                    if (reg >> 24) & 0xF:
                        features.add("result_shift")
                else:
                    mod = (reg >> 24) & 0xF
                    if mod in SRC_MODS:
                        features.add("srcmod_" + SRC_MODS[mod])
                if relative:
                    target = {REG_CONST: "const", REG_INPUT: "input",
                              REG_OUTPUT: "output"}.get(reg_type, "other")
                    features.add("relative_%s_%s" % ("dst" if is_dst else "src", target))
                    # Shader model 2 and 3 put the address register in the next token.
                    if i + 1 < len(params):
                        addr = params[i + 1]
                        features.add("relative_via_" + ("aL" if _reg_type(addr) == REG_LOOP else "a0"))
                        i += 1
                i += 1
                index += 1
        pos += 1 + length
    if not ended:
        raise BytecodeError("no end token")
    if pos != len(tokens):
        features.add("trailing_dwords")
    used = {k for k, v in ops.items() if v > 0}
    for op in ("loop", "rep"):
        if op in used:
            features.add("loops")
    if used & {"if", "ifc", "else"}:
        features.add("branches")
    if used & {"call", "callnz"}:
        features.add("subroutines")
    return {
        "stage": stage,
        "model": "%s_%d_%d" % (stage, major, minor),
        "ops": {k: v for k, v in ops.items() if v > 0},
        "features": sorted(features),
        "samplers": {str(k): v for k, v in sorted(sampler_types.items())},
        "max_float_const": max_const,
        "inputs": len(inputs),
        "outputs": len(outputs),
        "dwords": len(tokens),
        "instructions": sum(v for v in ops.values() if v > 0) - ops.get("dcl", 0)
                        - ops.get("def", 0) - ops.get("defi", 0) - ops.get("defb", 0),
        "comment_dwords": comment_dwords,
    }


# --- predicted translator support -------------------------------------------
# From running each candidate over the op-by-op cases in cases.py
# (vkd3d-shader 2.0, MojoShader ad5dff8, dxbc-spirv f528e1e; 2026-11-05).
# "reject" means the translator stopped or wrote SPIR-V that spirv-val refuses;
# "risk" means it translates but the result can differ from D3D9 unless the
# Vulkan adapter adds something. A corpus run with run_translators.py gives
# the real numbers; these predictions only rank what to look at first.

def predict(info: dict) -> dict:
    ops = set(info["ops"])
    feats = set(info["features"])
    samplers = set(info["samplers"].values())
    out = {}

    reject, risk = [], []
    for op in ("sub", "lit", "dst", "logp", "expp", "sgn", "crs", "setp", "call",
               "callnz", "breakc", "cnd"):
        if op in ops:
            reject.append("op_" + op)
    if "predicated_instruction" in feats and "setp" not in ops:
        reject.append("predicated_instruction")
    if feats & {"vpos", "vface"} and info["inputs"] > 0:
        reject.append("vpos_vface_with_inputs")
    if "rsq" in ops or "log" in ops:
        risk.append("rsq_log_without_abs")
    if ops & {"mul", "mad", "dp3", "dp4", "m4x4", "m4x3", "m3x4", "m3x3", "m3x2", "lrp"}:
        risk.append("no_d3d9_zero_times_inf")
    if ops & {"texldl", "texldd"}:
        risk.append("texldl_texldd_ignore_shadow_compare")
    out["vkd3d-shader"] = {"reject": reject, "risk": risk}

    reject, risk = [], []
    if "predicated_instruction" in feats:
        reject.append("predicated_instruction")
    if feats & {"relative_src_input", "relative_dst_output"}:
        reject.append("relative_input_or_output")
    if "centroid" in feats:
        reject.append("centroid")
    if "texldp" in ops and "cube" in samplers:
        reject.append("texldp_cube")
    for op in ("texldd", "break", "breakp", "callnz", "sgn", "call"):
        if op in ops:
            reject.append("op_" + op)
    if "relative_src_const" in feats and "def_float" in feats:
        risk.append("relative_const_with_def")
    if info["stage"] == "ps" and ops & {"texld", "texldp", "texldl", "texldd", "texldb"}:
        risk.append("no_shadow_compare")
    if "vpos" in feats:
        risk.append("vpos_half_pixel")
    if ops & {"mul", "mad", "dp3", "dp4", "m4x4", "m4x3", "m3x4", "m3x3", "m3x2", "lrp"}:
        risk.append("no_d3d9_zero_times_inf")
    out["mojoshader"] = {"reject": reject, "risk": risk}

    reject = ["op_breakp"] if "breakp" in ops else []
    out["dxbc-spirv"] = {"reject": reject, "risk": []}
    return out


# --- fastfile scanning -------------------------------------------------------

def inflate_fastfile(path: str) -> bytes:
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[:8] not in FASTFILE_MAGICS:
            raise ValueError("%s: not a CoD4 fastfile" % path)
        version = struct.unpack("<I", head[8:12])[0]
        if version != FASTFILE_VERSION:
            raise ValueError("%s: fastfile version %d, expected %d" % (path, version, FASTFILE_VERSION))
        if head[:8] != b"IWffu100":
            raise ValueError("%s: signed fastfiles are not read" % path)
        inflater = zlib.decompressobj()
        chunks = []
        while True:
            block = f.read(1 << 20)
            if not block:
                break
            chunks.append(inflater.decompress(block))
        chunks.append(inflater.flush())
    return b"".join(chunks)


_STRUCT_TAIL = b"\xff\xff\xff\xff"


def find_shaders(zone: bytes):
    """Yield (name, loadForRenderer, bytecode) for each inline shader."""
    start = 0
    while True:
        at = zone.find(_STRUCT_TAIL, start)
        if at < 0:
            return
        start = at + 1
        # Struct layout: [name -1][handle][program -1][size u16][renderer u16]
        if at + 16 > len(zone):
            return
        if zone[at + 8: at + 12] != _STRUCT_TAIL:
            continue
        size, renderer = struct.unpack_from("<HH", zone, at + 12)
        if size < 2 or renderer > 3:
            continue
        name_start = at + 16
        name_end = zone.find(b"\x00", name_start, name_start + 256)
        if name_end <= name_start:
            continue
        raw_name = zone[name_start:name_end]
        if not all(32 < c < 127 for c in raw_name):
            continue
        code_start = name_end + 1
        code_end = code_start + size * 4
        if code_end > len(zone):
            continue
        version = struct.unpack_from("<I", zone, code_start)[0]
        if version >> 16 not in (0xFFFE, 0xFFFF):
            continue
        if struct.unpack_from("<I", zone, code_end - 4)[0] != 0x0000FFFF:
            continue
        yield raw_name.decode("ascii"), renderer, zone[code_start:code_end]
        start = code_end


def resolve_groups(group_args):
    seen = set()
    groups = []
    for arg in group_args:
        if "=" not in arg:
            raise SystemExit("--group wants LABEL=GLOB, got %r" % arg)
        label, pattern = arg.split("=", 1)
        files = []
        for path in sorted(glob.glob(os.path.expanduser(pattern))):
            real = os.path.realpath(path)
            if real in seen or not os.path.isfile(path):
                continue
            seen.add(real)
            files.append(path)
        groups.append((label, files))
    return groups


def check_dump_dir(path: str) -> None:
    repo = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    real = os.path.realpath(path)
    if real == repo or real.startswith(repo + os.sep):
        raise SystemExit("--dump-dir must be outside the repository (game bytecode stays out of git)")


def dump_stage(code: bytes, error: str):
    """'vs' or 'ps' for a shader model 1.x program the parser skipped, else None."""
    if not error or not error.startswith("shader model 1."):
        return None
    return "vs" if struct.unpack_from("<I", code)[0] >> 16 == 0xFFFE else "ps"


def build_report(groups, dump_dir=None):
    shaders = {}          # sha -> record
    zones = []
    for label, files in groups:
        for path in files:
            entry = {"group": label, "zone": os.path.basename(path), "shaders": 0, "unique_new": 0}
            try:
                data = inflate_fastfile(path)
            except (ValueError, zlib.error, OSError) as exc:
                entry["error"] = str(exc)
                zones.append(entry)
                continue
            for name, renderer, code in find_shaders(data):
                entry["shaders"] += 1
                sha = hashlib.sha256(code).hexdigest()
                rec = shaders.get(sha)
                if rec is None:
                    entry["unique_new"] += 1
                    try:
                        info = parse_bytecode(code)
                        error = None
                    except BytecodeError as exc:
                        info, error = None, str(exc)
                    rec = {"info": info, "error": error, "groups": set(), "renderers": set(),
                           "families": collections.Counter(), "prefixes": collections.Counter()}
                    shaders[sha] = rec
                    # Shader model 1.x is not parsed here but is still dumped, so
                    # translator runs cover every shader the game has.
                    stage = info["stage"] if info else dump_stage(code, error)
                    if dump_dir and stage:
                        os.makedirs(dump_dir, exist_ok=True)
                        with open(os.path.join(dump_dir, "%s.%s" % (sha[:20], stage)), "wb") as f:
                            f.write(code)
                rec["groups"].add(label)
                rec["renderers"].add(renderer)
                rec["families"][classify_family(name)] += 1
                rec["prefixes"][name_prefix(name)] += 1
            zones.append(entry)
    return summarize(shaders, zones)


def summarize(shaders, zones):
    report = {
        "zones": zones,
        "unique_shaders": len(shaders),
        "parse_errors": collections.Counter(),
        "by_model": collections.Counter(),
        "by_renderer": collections.Counter(),
        "by_group_only": collections.Counter(),
        "by_family": collections.Counter(),
        "by_prefix": collections.Counter(),
        "op_shader_counts": collections.Counter(),
        "feature_shader_counts": collections.Counter(),
        "sampler_type_counts": collections.Counter(),
        "max_float_const_hist": collections.Counter(),
        "instructions": {"min": None, "max": 0, "total": 0},
        "predicted": {},
    }
    pred = collections.defaultdict(lambda: {"accept": 0, "reject": 0, "risk": 0,
                                            "reject_reasons": collections.Counter(),
                                            "risk_reasons": collections.Counter(),
                                            "reject_by_family": collections.Counter()})
    for sha, rec in shaders.items():
        info = rec["info"]
        family = rec["families"].most_common(1)[0][0]
        report["by_family"][family] += 1
        report["by_prefix"][rec["prefixes"].most_common(1)[0][0]] += 1
        groups = sorted(rec["groups"])
        report["by_group_only"]["+".join(groups)] += 1
        for r in rec["renderers"]:
            report["by_renderer"]["renderer_%d" % r] += 1
        if info is None:
            report["parse_errors"][rec["error"].split(" at ")[0]] += 1
            continue
        report["by_model"][info["model"]] += 1
        for op in info["ops"]:
            report["op_shader_counts"][op] += 1
        for feat in info["features"]:
            report["feature_shader_counts"][feat] += 1
        for kind in info["samplers"].values():
            report["sampler_type_counts"][info["stage"] + "_" + kind] += 1
        bucket = "none" if info["max_float_const"] < 0 else "c%d-c%d" % (
            info["max_float_const"] // 32 * 32, info["max_float_const"] // 32 * 32 + 31)
        report["max_float_const_hist"][info["stage"] + " " + bucket] += 1
        n = info["instructions"]
        inst = report["instructions"]
        inst["min"] = n if inst["min"] is None else min(inst["min"], n)
        inst["max"] = max(inst["max"], n)
        inst["total"] += n
        for translator, verdict in predict(info).items():
            p = pred[translator]
            if verdict["reject"]:
                p["reject"] += 1
                p["reject_by_family"][family] += 1
                for reason in verdict["reject"]:
                    p["reject_reasons"][reason] += 1
            else:
                p["accept"] += 1
            if verdict["risk"]:
                p["risk"] += 1
                for reason in verdict["risk"]:
                    p["risk_reasons"][reason] += 1
    report["predicted"] = {k: v for k, v in sorted(pred.items())}
    return report


def to_jsonable(obj):
    if isinstance(obj, collections.Counter):
        return {k: v for k, v in sorted(obj.items(), key=lambda kv: (-kv[1], str(kv[0])))}
    if isinstance(obj, dict):
        return {k: to_jsonable(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [to_jsonable(v) for v in obj]
    if isinstance(obj, set):
        return sorted(obj)
    return obj


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--group", action="append", default=[],
                        help="LABEL=GLOB of fastfiles; repeat for several groups")
    parser.add_argument("--out", help="write the JSON report here (default: stdout)")
    parser.add_argument("--dump-dir", help="also write unique bytecode here (outside the repo)")
    parser.add_argument("--no-prefixes", action="store_true",
                        help="leave shader name prefixes out of the report")
    args = parser.parse_args(argv)
    if not args.group:
        parser.error("at least one --group is required")
    if args.dump_dir:
        check_dump_dir(args.dump_dir)
    report = build_report(resolve_groups(args.group), args.dump_dir)
    if args.no_prefixes:
        report.pop("by_prefix")
    text = json.dumps(to_jsonable(report), indent=2)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
