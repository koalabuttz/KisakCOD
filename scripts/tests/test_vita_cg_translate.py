#!/usr/bin/env python3
"""Check the PS Vita back end's D3D9-to-Cg shader translator on the host.

``src/gfx_gxm/shader/gxm_cg_translate.cpp`` turns the game's D3D9 shader
bytecode into Cg for the Vita's own shader compiler. That compiler only runs
on a Vita, so this test checks what can be checked here:

1. it builds the translator with ``tests/vita_gxm/cg_translate_tool.cpp``
   (ASan and UBSan on);
2. it assembles the shader spike's op-by-op cases
   (``scripts/tools/shader_spike/cases.py``) and translates each one: the
   cases listed in REFUSED must be refused with a reason, every other one
   must translate;
3. when glslangValidator is installed, it compiles every program with
   glslang's HLSL front end. Cg and DX9 HLSL share the syntax the
   translator writes, so this catches malformed or mistyped output.
   ``--require-glslang`` makes a missing glslang a failure (CI);
4. a few cases are checked for the options' effect (D3DCOLOR inputs, alpha
   test) and the reflection (constant counts, varyings), and every pixel
   case is translated again with the half temporaries experiment on;
5. truncated and corrupted bytecode must be refused without a crash.

Usage:
    python3 scripts/tests/test_vita_cg_translate.py [--cxx c++] [--require-glslang]
"""

import argparse
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "tools" / "shader_spike"))

import cases  # noqa: E402
import d3d9_asm  # noqa: E402

SOURCES = [
    ROOT / "src" / "gfx_gxm" / "shader" / "gxm_cg_translate.cpp",
    ROOT / "tests" / "vita_gxm" / "cg_translate_tool.cpp",
]

# Cases the translator refuses, and a word its reason must contain. The
# game's shaders use none of these (renderer plan S0's corpus scan).
REFUSED = {
    "ps3_setp_predicated": "predicated",
    "ps3_predicate_not": "predicated",
    "ps3_if_bool": "if",
    "ps3_if_predicate": "setp",
    "ps3_loop_al": "loop",
    "ps3_rep": "rep",
    "ps3_defi_rep": "rep",
    "ps3_break": "rep",
    "ps3_breakc": "rep",
    "ps3_breakp": "rep",
    "ps3_call_label": "call",
    "ps3_callnz_bool": "callnz",
    "ps3_relative_input_al": "loop",
    "ps3_relative_const_al": "loop",
    "ps3_vpos": "vPos",
    "ps3_vface": "vFace",
    "ps3_mrt": "oC1",
    "vs3_loop_relative": "loop",
    "vs3_bool_if": "if",
    "vs3_defb_if": "if",
    "vs3_output_relative": "loop",
    "ps2x_dsx_predicate": "predicated",
    "vs2_loop": "loop",
}

# Cases of this test alone. HLSL writes float4(v.xyz, 1) as a mad with a
# def'd constant's zeros and ones; the Vita's compiler folds x * 0 + c to 0
# (the menu drew nothing until a console run showed it), so the translator
# keeps products by a constant's zero out of the program. It also reads every
# def'd constant as a literal of the swizzled components: the compiler got
# lanes of a swizzled constant wrong in a decal shader too (PC run Z7).
VITA_CASES = {
    "vs3_vita_mad_zero": "vs_3_0\ndef c8, 1, 0, 0, 0\ndef c9, 0, 2, 0, 3\ndcl_position v0\ndcl_position o0\n"
                         "dcl_texcoord o1\nmad r0, v0.xyzx, c8.xxxy, c8.yyyx\nmul r1, v0, c8.yxyx\n"
                         "mad r2.xz, v0, c8.xxyy, c9.yyww\nmul r3.yw, c9, v0\nmad r4, r0, -c8.y, r0.wzyx\n"
                         "mad r5, v0, c9.yyww, c1\nadd r0, r0, r1\nadd r2, r2, r3\nadd r4, r4, r5\n"
                         "add o0, r0, r2\nmov o1, r4",
    # A coordinate computed from a texture: r0 and r1 reach a coordinate,
    # r2 and r3 only the colour (--half-temporaries).
    "ps3_vita_dependent_read": "ps_3_0\ndcl_texcoord v0\ndcl_2d s0\ndcl_2d s1\ntexld r0, v0, s0\n"
                               "mad r1, r0, c4, v0\ntexld r2, r1, s1\nmul r3, r2, c5\nmax r3, r3, r0\n"
                               "mov oC0, r3",
}

# (case, extra tool arguments, text the program must contain, text the
# reflection line must contain)
CHECKS = [
    ("vs2_oT_oD_fog", ["--bgra", "2"], "float4 v1 = in_v1.zyxw;", "varyings=TEXCOORD0,COLOR0,FOG0"),
    ("vs2_oT_oD_fog", [], "float4 v1 = in_v1;", "constants=6"),
    ("vs2_oT_oD_fog", ["--absent", "2"], "float4 v1 = float4(0.0, 0.0, 0.0, 1.0);", "inputs=0:0.0,1:5.0"),
    ("vs2_oT_oD_fog", ["--components", "0=3,1=2"], "float4 v1 = float4(in_v1, 0.0, 1.0);", "inputs=0:0.0,1:5.0"),
    ("ps3_texkill", ["--alpha", "7"], "if (!(a >= ref)) discard;", "kills=1"),
    ("ps3_texkill", ["--alpha", "1"], "if (!(false)) discard;", "kills=1"),
    ("ps3_texkill", [], "return oC0;", "kills=1"),
    ("ps3_vita_dependent_read", [], "float4 r3 = float4(0.0, 0.0, 0.0, 0.0);", "samplers=0:2d,1:2d"),
    ("ps3_vita_dependent_read", ["--half-temporaries"],
     "    float4 r0 = float4(0.0, 0.0, 0.0, 0.0);\n    float4 r1 = float4(0.0, 0.0, 0.0, 0.0);\n"
     "    half4 r2 = float4(0.0, 0.0, 0.0, 0.0);\n    half4 r3 = float4(0.0, 0.0, 0.0, 0.0);\n"
     "    half4 oC0 = float4(0.0, 0.0, 0.0, 0.0);\n", "samplers=0:2d,1:2d"),
    ("ps3_vita_dependent_read", ["--half-temporaries"],
     "    r1 = (r0 * c[4] + v0);\n    r2 = tex2D(s1, (r1).xy);\n    r3 = (r2 * ((half4)(c[5])));\n"
     "    r3 = max(r3, ((half4)(r0)));\n    oC0 = r3;\n", "samplers=0:2d,1:2d"),
    ("vs2_oT_oD_fog", ["--half-temporaries"], "float4 v1 = in_v1;", "constants=6"),
    ("ps3_def_const", [], "const float4 k20 = float4(0.5, 1.0, 2.0, -1.0);", "constants=0"),
    ("vs3_skinning_256", [], "c[a0.y + 16]", "constants=256 relative=1"),
    ("vs3_m4x3_m3x4_m3x2", [], "dot((v1).xyz, (c[13]).xyz)", "constants=14"),
    ("ps2_texldp_color", [], "tex2D(s0, (t0).xy / (t0).w)", "varyings=TEXCOORD0,COLOR0"),
    ("ps3_texld_cube", [], "texCUBE(s0, (v0).xyz)", "samplers=0:cube"),
    ("ps3_texld_volume", [], "kisak_tex3D(s0, (v0).xyz, kisak_depth_s0)", "samplers=0:volume"),
    ("vs3_psize_fog", [], "out_FOG = o3.x;", "varyings=TEXCOORD0,FOG0,PSIZE0"),
    ("ps3_odepth", [], "return oC0;", "instructions=3"),
    ("vs2_oT_oD_fog", ["--far-plane-inside"], "out_POSITION.z = min(out_POSITION.z, out_POSITION.w * 0.99999);",
     "constants=6"),
    ("vs2_oT_oD_fog", ["--diag-position", "1"], "out_POSITION = float4(v0.x / 480.0 - 1.0, 1.0 - v0.y / 272.0, 0.5, 1.0);",
     "constants=6"),
    ("vs2_oT_oD_fog", ["--diag-position", "2"], "out_POSITION = float4(dot(v0, c[0]), dot(v0, c[1]), 0.5, 1.0);",
     "constants=6"),
    ("vs3_psize_fog", ["--diag-position", "3"], "out_POSITION.zw = float2(0.5, 1.0);", "constants=5"),
    ("vs3_psize_fog", ["--diag-position", "4"], "out_POSITION.z = out_POSITION.w * 0.5;", "constants=5"),
    ("vs3_vita_mad_zero", [], "r0 = float4((v0.xyzx * float4(1.0, 1.0, 1.0, 0.0) + float4(0.0, 0.0, 0.0, 1.0)).xyz, "
     "(float4(0.0, 0.0, 0.0, 1.0)).w);", "constants=2"),
    ("vs3_vita_mad_zero", [], "r1 = float4(0.0, (v0 * float4(0.0, 1.0, 0.0, 1.0)).y, 0.0, (v0 * float4(0.0, 1.0, 0.0, 1.0)).w);", "instructions=11"),
    ("vs3_vita_mad_zero", [], "r2.xz = (float4((v0 * float4(1.0, 1.0, 0.0, 0.0) + float4(2.0, 2.0, 3.0, 3.0)).xy, "
     "(float4(2.0, 2.0, 3.0, 3.0)).zw)).xz;", "constants=2"),
    ("vs3_vita_mad_zero", [], "r3.yw = ((float4(0.0, 2.0, 0.0, 3.0) * v0)).yw;", "constants=2"),
    ("vs3_vita_mad_zero", [], "r4 = float4((r0.wzyx).xyzw);", "constants=2"),
    ("vs3_vita_mad_zero", [], "r5 = (v0 * float4(2.0, 2.0, 3.0, 3.0) + c[1]);", "constants=2"),
    ("vs2_oT_oD_fog", ["--diag-position", "5"],
     "out_TEXCOORD0 = float4(out_POSITION.x * 0.5 + 0.5, out_POSITION.y * 0.5 + 0.5, out_POSITION.w * 0.5, 1.0);\n"
     "    out_POSITION = float4(v0.x / 480.0 - 1.0, 1.0 - v0.y / 272.0, 0.5, 1.0);", "constants=6"),
    ("vs2_oT_oD_fog", ["--diag-position", "8"],
     "out_TEXCOORD0 = float4(out_POSITION.z * 0.25 + 0.5, out_POSITION.w * 0.25 + 0.5, 0.25, 1.0);", "constants=6"),
]


def run_tool(tool, path, extra=()):
    return subprocess.run([str(tool), *extra, str(path)], capture_output=True, text=True, timeout=60)


def glslang_ok(glslang, stage, path):
    result = subprocess.run([glslang, "-D", "-S", stage, "-e", "main", "--hlsl-dx9-compatible", "-V",
                             str(path), "-o", str(path) + ".spv"], capture_output=True, text=True, timeout=60)
    return result.returncode == 0, result.stdout + result.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--require-glslang", action="store_true")
    parser.add_argument("--keep", type=Path, help="write the programs to this folder")
    args = parser.parse_args()
    glslang = shutil.which("glslangValidator")
    if args.require_glslang and not glslang:
        print("FAIL: glslangValidator is not installed (apt install glslang-tools)")
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="kisak-cg-translate-") as work_dir:
        work = Path(work_dir)
        tool = work / "cg_translate_tool"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(tool)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=300)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the translator did not build")
            return 1

        translated = refused = compiled = 0
        binaries = {}
        for name, source in {**cases.all_cases(), **VITA_CASES}.items():
            binary = work / (name + ".bin")
            binary.write_bytes(d3d9_asm.assemble(source))
            binaries[name] = binary
            result = run_tool(tool, binary)
            if name in REFUSED:
                word = REFUSED[name]
                if result.returncode != 2 or word not in result.stderr:
                    failures.append(f"{name}: expected a refusal naming '{word}', got exit "
                                    f"{result.returncode}: {result.stderr.strip()}")
                else:
                    refused += 1
                continue
            if result.returncode:
                failures.append(f"{name}: not translated: {result.stderr.strip()}")
                continue
            translated += 1
            # The Vita's Cg compiler finds lerp(tex2D(...), tex2D(...), w)
            # ambiguous (error 5476) though glslang accepts it; run C failed
            # every shader on it.
            if "lerp(tex2D" in result.stdout:
                failures.append(f"{name}: lerp over tex2D results, which the Vita compiler refuses")
            program = work / (name + ".cg")
            program.write_text(result.stdout)
            if args.keep:
                args.keep.mkdir(parents=True, exist_ok=True)
                (args.keep / (name + ".cg")).write_text(result.stdout)
            if glslang:
                ok, log = glslang_ok(glslang, "vert" if name.startswith("vs") else "frag", program)
                if ok:
                    compiled += 1
                else:
                    failures.append(f"{name}: glslang rejects the program:\n{log.strip()}")

        for name, extra, text, reflection in CHECKS:
            result = run_tool(tool, binaries[name], extra)
            if result.returncode:
                failures.append(f"{name} {extra}: not translated: {result.stderr.strip()}")
                continue
            if text not in result.stdout:
                failures.append(f"{name} {extra}: the program lacks {text!r}")
            if reflection not in result.stderr:
                failures.append(f"{name} {extra}: the reflection lacks {reflection!r}: {result.stderr.strip()}")
            if glslang and extra:
                program = work / (name + "-options.cg")
                program.write_text(result.stdout)
                ok, log = glslang_ok(glslang, "vert" if name.startswith("vs") else "frag", program)
                if not ok:
                    failures.append(f"{name} {extra}: glslang rejects the program:\n{log.strip()}")

        # The half temporaries experiment must leave every pixel shader a
        # program, with a half register in it, and vertex shaders alone.
        halved = 0
        for name, binary in binaries.items():
            if name in REFUSED:
                continue
            plain = run_tool(tool, binary)
            result = run_tool(tool, binary, ["--half-temporaries"])
            if result.returncode:
                failures.append(f"{name} --half-temporaries: not translated: {result.stderr.strip()}")
                continue
            if name.startswith("vs"):
                if result.stdout != plain.stdout:
                    failures.append(f"{name} --half-temporaries: a vertex program changed")
                continue
            if "half4 oC0" not in result.stdout and "half4 r" not in result.stdout:
                failures.append(f"{name} --half-temporaries: no half register in the program")
            halved += 1
            if glslang:
                program = work / (name + "-half.cg")
                program.write_text(result.stdout)
                ok, log = glslang_ok(glslang, "frag", program)
                if not ok:
                    failures.append(f"{name} --half-temporaries: glslang rejects the program:\n{log.strip()}")

        # Damaged bytecode: refused or translated, never a crash.
        rng = random.Random(20261109)
        damaged = 0
        for name, binary in binaries.items():
            data = binary.read_bytes()
            for trial in range(12):
                blob = bytearray(data)
                if trial < 4:
                    blob = blob[: rng.randrange(0, len(blob) // 4) * 4]
                else:
                    for _ in range(rng.randint(1, 4)):
                        at = rng.randrange(len(blob))
                        blob[at] ^= 1 << rng.randrange(8)
                path = work / "damaged.bin"
                path.write_bytes(bytes(blob))
                result = run_tool(tool, path)
                damaged += 1
                if result.returncode not in (0, 2):
                    failures.append(f"{name} damaged #{trial}: exit {result.returncode}: "
                                    f"{result.stderr.strip()[-300:]}")
                    break

    for failure in failures:
        print("FAIL:", failure)
    glslang_note = f"{compiled} accepted by glslang" if glslang else "glslang not installed, syntax not checked"
    print(f"{translated} translated ({glslang_note}), {refused} refused as expected, "
          f"{len(CHECKS)} option checks, {halved} with half temporaries, {damaged} damaged inputs")
    if failures:
        return 1
    print("PASS: gxm cg translator")
    return 0


if __name__ == "__main__":
    sys.exit(main())
