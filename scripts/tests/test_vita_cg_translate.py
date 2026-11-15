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
   test) and the reflection (constant counts, varyings);
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
    "ps1_vita_coissue_reads_partner": "runs beside",
    "ps1_vita_bump": "ps 1.x opcode 67",
    "ps1_vita_model_4": "ps_1_4",
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
    # The oldest pixel shader model, which the game's shader model 2 set
    # draws its depth pass with: a texture instruction names one register,
    # the colour is what r0 holds at the end, an instruction may run beside
    # the one before it.
    "ps1_vita_constant": "ps_1_1\nmov r0, c0",
    "ps1_vita_texture": "ps_1_1\ntex t0\nmov r0, t0",
    "ps1_vita_modulate": "ps_1_1\ndef c7, 0.25, 0.5, 0.75, 1\ntex t0\ntex t1\nmul r0.rgb, t0, v0\n"
                         "+mov r0.a, t0.a\nmul_x2 r1, t1_bx2, c7\nlrp_sat r0.rgb, v1.a, r0, r1\n"
                         "mad r0.a, t1.b, c1_comp.a, r0.a",
    "ps1_vita_texcoord_kill": "ps_1_3\ntexcoord t0\ntexkill t1\ntex t2\ndp3_sat r1, t0_bx2, t2_bx2\n"
                              "mul r0, r1, t2\ncnd r0, r0.a, r0, c2\nadd_d2 r0.rgb, r0, t0",
    "ps1_vita_coissue_reads_partner": "ps_1_1\ntex t0\nmul r0.rgb, t0, v0\n+mov r0.a, r0.b",
    "ps1_vita_coissue_other_lanes": "ps_1_1\ntex t0\nmov r0, t0\nmul r0.rgb, r0, v0\n+mul r0.a, r0.a, v0.a",
    "ps1_vita_bump": "ps_1_1\ntex t0\ntexbem t1, t0\nmov r0, t1",
    "ps1_vita_model_4": "ps_1_4\ntexld r0, t0",
    # Texture reads whose coordinates the vertex program can work out
    # (PlanHoist): the last two components of an input, alone, times a
    # literal constant, and scaled and moved through a temporary. The reads
    # of s1 are not: one at the input's own first two components, one that
    # depends on another read, one scaled by a constant the game sets.
    "ps3_vita_hoist": "ps_3_0\ndef c2, 0.5, 0.25, 0.125, 2\ndcl_texcoord0 v0\ndcl_texcoord1 v1.xy\n"
                      "dcl_2d s0\ndcl_2d s1\ndcl_2d s2\ntexld r0, v0, s1\ntexld r1, v0.zwzw, s2\n"
                      "mul r2.xy, v0.zwzw, c2\ntexld r2, r2, s0\nmad r3, v0, c2.xyxy, c2.xyzy\n"
                      "mov r3.xy, r3.zwzw\ntexld r3, r3, s0\nmul r4.xy, v0.zwzw, c2\ntexld r4, r4, s0\n"
                      "add r5.xy, r0, v1\ntexld r5, r5, s1\nmul r6.xy, v1, c5\ntexld r6, r6, s1\n"
                      "add r0, r0, r1\nadd r0, r0, r2\nadd r0, r0, r3\nadd r0, r0, r4\nadd r0, r0, r5\n"
                      "add r0, r0, r6\nmov oC0, r0",
    "vs3_vita_hoist": "vs_3_0\ndcl_position v0\ndcl_texcoord v1\ndcl_position o0\ndcl_texcoord0 o1\n"
                      "dcl_texcoord1 o2.xy\nmov o0, v0\nmov o1, v1\nmov o2.xy, v1",
    # A vertex shader that writes TEXCOORD9 itself, and one that never
    # writes TEXCOORD0: neither can feed the plan.
    "vs3_vita_hoist_taken": "vs_3_0\ndcl_position v0\ndcl_texcoord v1\ndcl_position o0\ndcl_texcoord0 o1\n"
                            "dcl_texcoord9 o2\nmov o0, v0\nmov o1, v1\nmov o2, v1",
    "vs3_vita_hoist_unfed": "vs_3_0\ndcl_position v0\ndcl_texcoord v1\ndcl_position o0\ndcl_texcoord0 o1\n"
                            "dcl_texcoord1 o2\nmov o0, v0\nmov o2, v1",
    "ps2_vita_hoist": "ps_2_0\ndef c0, 2, 3, 0, 0\ndcl t0\ndcl t1\ndcl_2d s0\nmov r0.xy, t0.wzyx\n"
                      "texld r0, r0, s0\nadd r1, t1, -c0.xyxy\ntexld r1, r1, s0\nadd r0, r0, r1\nmov oC0, r0",
    "vs2_vita_hoist": "vs_2_0\ndcl_position v0\ndcl_texcoord v1\nmov oPos, v0\nmov oT0, v1\nmov oT1, v1",
}

# (case, extra tool arguments, text the program must contain, text the
# reflection line must contain)
CHECKS = [
    ("ps1_vita_constant", [], "    oC0 = r0;\n", "constants=1 "),
    ("ps1_vita_constant", ["--alpha", "5"], "if (!(a > ref)) discard;", "varyings= "),
    ("ps1_vita_constant", ["--alpha", "5", "--alpha-zero"],
     "if (!(a > ref)) oC0 = float4(0.0, 0.0, 0.0, 0.0);\n    return oC0;", "varyings= "),
    ("ps1_vita_texture", [], "    r2 = tex2D(s0, (t0).xy);\n    r0 = r2;\n    oC0 = r0;\n",
     "samplers=0:2d"),
    ("ps1_vita_texture", [], "float4 in_TEXCOORD0 : TEXCOORD0", "varyings=TEXCOORD0"),
    # The sharpening read of a scaled picture: only the samplers asked for,
    # and nothing of it in a program that did not ask.
    ("ps1_vita_texture", ["--sharp", "1"], "    r2 = kisak_sharp2D(s0, (t0).xy);\n", "samplers=0:2d"),
    ("ps1_vita_texture", ["--sharp", "1"], "uniform float4 kisak_sharp_max;\nfloat4 kisak_sharp2D(sampler2D s, float2 uv)",
     "samplers=0:2d"),
    ("ps1_vita_texture", ["--sharp", "2"], "    r2 = tex2D(s0, (t0).xy);\n", "samplers=0:2d"),
    ("ps1_vita_modulate", [], "    r0.xyz = ((r2 * v0)).xyz;\n    r0.w = (r2.wwww).w;\n", "samplers=0:2d,1:2d"),
    ("ps1_vita_modulate", [], "r1 = (((r3 * 2.0 - 1.0) * float4(0.25, 0.5, 0.75, 1.0)) * 2.0);",
     "varyings=TEXCOORD0,TEXCOORD1,COLOR0,COLOR1"),
    ("ps1_vita_modulate", [], "(1.0 - c[1].wwww)", "constants=2 "),
    ("ps1_vita_texcoord_kill", [], "    r2.xyz = (saturate(t0)).xyz;\n    r2.w = (float4(1.0, 1.0, 1.0, 1.0)).w;\n",
     "kills=1"),
    ("ps1_vita_texcoord_kill", [], "if (t1.x < 0.0 || t1.y < 0.0 || t1.z < 0.0)", "samplers=2:2d"),
    ("ps1_vita_texcoord_kill", [], "r0.xyz = (((r0 + r2) * 0.5)).xyz;", "varyings=TEXCOORD0,TEXCOORD1,TEXCOORD2"),
    ("ps1_vita_coissue_other_lanes", [], "    r0.xyz = ((r0 * v0)).xyz;\n    r0.w = ((r0.wwww * v0.wwww)).w;\n",
     "samplers=0:2d"),
    ("vs2_oT_oD_fog", ["--bgra", "2"], "float4 v1 = in_v1.zyxw;", "varyings=TEXCOORD0,COLOR0,FOG0"),
    ("vs2_oT_oD_fog", [], "float4 v1 = in_v1;", "constants=6"),
    ("vs2_oT_oD_fog", ["--absent", "2"], "float4 v1 = float4(0.0, 0.0, 0.0, 1.0);", "inputs=0:0.0,1:5.0"),
    ("vs2_oT_oD_fog", ["--components", "0=3,1=2"], "float4 v1 = float4(in_v1, 0.0, 1.0);", "inputs=0:0.0,1:5.0"),
    ("ps3_texkill", ["--alpha", "7"], "if (!(a >= ref)) discard;", "kills=1"),
    ("ps3_texkill", ["--alpha", "1"], "if (!(false)) discard;", "kills=1"),
    ("ps3_texkill", [], "return oC0;", "kills=1"),
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
    # Hoisted reads. Three coordinates for four reads (two are the same);
    # the reads of s1 stay as they were.
    ("ps3_vita_hoist", [], "r2 = tex2D(s0, (r2).xy);", "instructions="),
    ("ps3_vita_hoist", ["--hoist"], "r1 = tex2D(s2, in_TEXCOORD9);", "hoist=3/4"),
    ("ps3_vita_hoist", ["--hoist"], "r2 = tex2D(s0, in_TEXCOORD8);", "hoist=3/4"),
    ("ps3_vita_hoist", ["--hoist"], "r3 = tex2D(s0, in_TEXCOORD7);", "hoist=3/4"),
    ("ps3_vita_hoist", ["--hoist"], "r4 = tex2D(s0, in_TEXCOORD8);", "hoist=3/4"),
    ("ps3_vita_hoist", ["--hoist"], "r0 = tex2D(s1, (v0).xy);", "hoist=3/4"),
    ("ps3_vita_hoist", ["--hoist"], "r5 = tex2D(s1, (r5).xy);", "hoist=3/4"),
    ("ps3_vita_hoist", ["--hoist"], "r6 = tex2D(s1, (r6).xy);", "hoist=3/4"),
    ("ps3_vita_hoist", ["--hoist"], "float2 in_TEXCOORD7 : TEXCOORD7", "hoist=3/4"),
    ("vs3_vita_hoist", ["--hoist-from", "@ps3_vita_hoist"],
     "    out_TEXCOORD9 = float2(out_TEXCOORD0.z, out_TEXCOORD0.w);\n"
     "    out_TEXCOORD8 = float2(out_TEXCOORD0.z * 0.5, out_TEXCOORD0.w * 0.25);\n"
     "    out_TEXCOORD7 = float2(out_TEXCOORD0.z * 0.5 + 0.125, out_TEXCOORD0.w * 0.25 + 0.25);\n", "hoist=3/4"),
    ("vs3_vita_hoist", ["--hoist-from", "@ps3_vita_hoist"], "out float2 out_TEXCOORD8 : TEXCOORD8", "hoist=3/4"),
    ("vs3_vita_hoist_taken", ["--hoist-from", "@ps3_vita_hoist"], "out_TEXCOORD9 = o2;", "hoist=no"),
    ("vs3_vita_hoist_unfed", ["--hoist-from", "@ps3_vita_hoist"], "out_TEXCOORD1 = o2;", "hoist=no"),
    ("vs3_vita_hoist", ["--hoist-from", "@ps3_texkill"], "out_TEXCOORD0 = o1;", "hoist=no"),
    ("ps3_texkill", ["--hoist"], "return oC0;", "hoist=no"),
    ("ps2_vita_hoist", ["--hoist"], "r0 = tex2D(s0, in_TEXCOORD9);", "hoist=2/2"),
    ("ps2_vita_hoist", ["--hoist"], "r1 = tex2D(s0, in_TEXCOORD8);", "hoist=2/2"),
    ("vs2_vita_hoist", ["--hoist-from", "@ps2_vita_hoist"],
     "    out_TEXCOORD9 = float2(out_TEXCOORD0.w, out_TEXCOORD0.z);\n"
     "    out_TEXCOORD8 = float2(out_TEXCOORD1.x + -2.0, out_TEXCOORD1.y + -3.0);\n", "hoist=2/2"),
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
            # "@case" stands for that case's bytecode file.
            extra = [str(binaries[word[1:]]) if word.startswith("@") else word for word in extra]
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
                # Half of them with the read hoist's plan made from them.
                result = run_tool(tool, path, ["--hoist"] if trial % 2 else [])
                damaged += 1
                if result.returncode not in (0, 2):
                    failures.append(f"{name} damaged #{trial}: exit {result.returncode}: "
                                    f"{result.stderr.strip()[-300:]}")
                    break

    for failure in failures:
        print("FAIL:", failure)
    glslang_note = f"{compiled} accepted by glslang" if glslang else "glslang not installed, syntax not checked"
    print(f"{translated} translated ({glslang_note}), {refused} refused as expected, "
          f"{len(CHECKS)} option checks, {damaged} damaged inputs")
    if failures:
        return 1
    print("PASS: gxm cg translator")
    return 0


if __name__ == "__main__":
    sys.exit(main())
