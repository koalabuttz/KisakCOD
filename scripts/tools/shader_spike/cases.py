#!/usr/bin/env python3
"""Op-by-op D3D9 test shaders for the shader translator spike (renderer plan S0).

Each case is a short shader in D3D assembly (see d3d9_asm.py) that exercises
one instruction or one D3D9 feature, written the way fxc emits it, and that
sends its result to an output so no translator can drop it as dead code.
They contain no game data.
"""

PS3_HEAD = """ps_3_0
dcl_texcoord0 v0
dcl_texcoord1 v1
"""
PS3_TAIL = "mov oC0, r0\n"

VS3_HEAD = """vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_texcoord0 o1
"""
VS3_TAIL = "m4x4 o0, v0, c0\nmov o1, r0\n"

# name: (stage, body). Bodies leave their result in r0.
PS3_BODIES = {
    "add": "add r0, v0, c4",
    "sub": "sub r0, v0, c4",
    "mul": "mul r0, v0, c4",
    "mad": "mad r0, v0, c4, c5",
    "rcp": "rcp r0, v0.x",
    "rsq": "rsq r0, v0.x",
    "dp3": "dp3 r0, v0, c4",
    "dp4": "dp4 r0, v0, c4",
    "min_max": "min r0, v0, c4\nmax r0, r0, c5",
    "exp": "exp r0, v0.x",
    "log": "log r0, v0.x",
    "lrp": "lrp r0, v0, c4, c5",
    "frc": "frc r0, v0",
    "pow": "pow r0, v0.x, c4.x",
    "crs": "mov r0, c5\ncrs r0.xyz, v0, c4",
    "abs": "abs r0, v0",
    "nrm": "nrm r0, v0",
    "sincos": "mov r0, c5\nsincos r0.xy, v0.x",
    "cmp": "cmp r0, v0, c4, c5",
    "dp2add": "dp2add r0, v0, c4, c5.x",
    "dsx_dsy": "dsx r1, v0\ndsy r2, v0\nadd r0, r1, r2",
    "m4x4": "m4x4 r0, v0, c4",
    "m3x3": "mov r0, c8\nm3x3 r0.xyz, v0, c4",
    "srcmod_neg_abs": "add r0, -v0, |c4|",
    "dstmod_sat_pp": "mul_sat_pp r0, v0, c4",
    "def_const": "def c20, 0.5, 1, 2, -1\nmul r0, v0, c20",
    "texld_2d": "dcl_2d s0\ntexld r0, v0, s0",
    "texld_cube": "dcl_cube s0\ntexld r0, v0, s0",
    "texld_volume": "dcl_volume s0\ntexld r0, v0, s0",
    "texldp": "dcl_2d s0\ntexldp r0, v0, s0",
    "texldb": "dcl_2d s0\ntexldb r0, v0, s0",
    "texldl": "dcl_2d s0\ntexldl r0, v0, s0",
    "texldd": "dcl_2d s0\ntexldd r0, v0, s0, v1, c4",
    "texkill": "sub r1, v0, c4\ntexkill r1\nmov r0, c4",
    "texkill_input": "texkill v0\nmov r0, c4",
    "add_neg_for_sub": "add r0, v0, -c4",
    "texld_16_samplers": "\n".join("dcl_2d s%d" % i for i in range(16)) + "\n" +
                         "texld r0, v0, s0\n" +
                         "\n".join("texld r1, v0, s%d\nadd r0, r0, r1" % i for i in range(1, 16)),
    "setp_predicated": "setp_gt p0, v0, c4\nmov r0, c5\n(p0) mov r0, v0",
    "predicate_not": "setp_lt p0, v0, c4\nmov r0, c5\n(!p0) add r0, v0, c4",
    "if_bool": "if b0\nmov r0, v0\nelse\nmov r0, c4\nendif",
    "ifc": "ifc_gt v0.x, c4.x\nmov r0, v0\nelse\nmov r0, c4\nendif",
    "if_predicate": "setp_ge p0.x, v0.x, c4.x\nif p0.x\nmov r0, v0\nelse\nmov r0, c4\nendif",
    "loop_al": "mov r0, c4\nloop aL, i0\nadd r0, r0, v0\nendloop",
    "rep": "mov r0, c4\nrep i0\nadd r0, r0, v0\nendrep",
    "defi_rep": "defi i1, 4, 0, 0, 0\nmov r0, c4\nrep i1\nadd r0, r0, v0\nendrep",
    "break": "mov r0, c4\nrep i0\nadd r0, r0, v0\nbreak\nendrep",
    "breakc": "mov r0, c4\nrep i0\nadd r0, r0, v0\nbreakc_gt r0.x, c5.x\nendrep",
    "breakp": "mov r0, c4\nrep i0\nadd r0, r0, v0\nsetp_gt p0.x, r0.x, c5.x\nbreakp p0.x\nendrep",
    "call_label": "call l0\nmov r0, r1\nret\nlabel l0\nmul r1, v0, c4\nret",
    "callnz_bool": "mov r1, c4\ncallnz l0, b0\nmov r0, r1\nret\nlabel l0\nmul r1, v0, c4\nret",
    "relative_input_al": "dcl_texcoord2 v2\nmov r0, c4\nloop aL, i0\nadd r0, r0, v[aL + 0]\nendloop",
    "relative_const_al": "mov r0, c4\nloop aL, i0\nadd r0, r0, c[aL + 8]\nendloop",
    "vpos": "dcl vPos.xy\nmul r0, vPos.xyxy, c4",
    "vface": "dcl vFace\ncmp r0, vFace, c4, c5",
    "odepth": "mov r0, v0\nmov oDepth, v0.x",
    "mrt": "mov r0, v0\nmov oC1, c4\nmov oC2, c5\nmov oC3, v1",
    "centroid_input": "dcl_texcoord2_centroid v2\nmul r0, v2, c4",
    "color_input": "dcl_color0 v2\nmul r0, v2, c4",
}

VS3_BODIES = {
    "vs_arith": "mad r0, v1, c4, c5\ndp3 r1.x, v1, c6\nadd r0, r0, r1.x",
    "vs_lit": "lit r0, v1",
    "vs_dst": "dst r0, v1, c4",
    "vs_expp_logp": "expp r1, v1.x\nlogp r2, v1.y\nadd r0, r1, r2",
    "vs_sgn": "sgn r0, v1, r1, r2",
    "vs_slt_sge": "slt r1, v1, c4\nsge r2, v1, c4\nadd r0, r1, r2",
    "vs_mova_relative": "mova a0.x, v1.x\nmov r0, c[a0.x + 8]",
    "vs_mova_relative_def": "def c9, 1, 2, 3, 4\nmova a0.x, v1.x\nmov r0, c[a0.x + 8]",
    "vs_loop_relative": "mov r0, c4\nloop aL, i0\nadd r0, r0, c[aL + 8]\nendloop",
    "vs_m4x3_m3x4_m3x2": "m4x3 r1.xyz, v1, c4\nm3x4 r2, v1, c8\nm3x2 r3.xy, v1, c12\nadd r0, r2, r1.xyzz\nadd r0.xy, r0, r3",
    "vs_texldl": "dcl_2d s0\ntexldl r0, v1, s0",
    "vs_bool_if": "if b1\nmov r0, v1\nelse\nmov r0, c4\nendif",
    "vs_defb_if": "defb b2, true\nif b2\nmov r0, v1\nelse\nmov r0, c4\nendif",
    "vs_skinning_256": "dcl_blendindices v2\ndcl_blendweight v3\nmul r1, v2, c4.x\nmova a0, r1\nm4x3 r2.xyz, v0, c[a0.x + 16]\nm4x3 r3.xyz, v0, c[a0.y + 16]\nmul r2.xyz, r2, v3.x\nmad r0.xyz, r3, v3.y, r2\nmov r0.w, c4.y\nadd r0, r0, c255",
    "vs_output_relative": "dcl_texcoord1 o2\ndcl_texcoord2 o3\nmov r0, v1\nloop aL, i0\nmov o[aL + 2], v1\nendloop",
    "vs_psize_fog": "dcl_psize o2\ndcl_fog o3\nmov r0, v1\nmov o2.x, c4.x\nmov o3.x, c4.y",
}

PS2_CASES = {
    "ps2_texld": "ps_2_0\ndcl t0\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0",
    "ps2_texldp_color": "ps_2_0\ndcl t0\ndcl v0\ndcl_2d s0\ntexldp r0, t0, s0\nmul r0, r0, v0\nmov oC0, r0",
    "ps2_texkill_cmp": "ps_2_0\ndcl t0\ntexkill t0\ncmp r0, t0, c0, c1\nmov oC0, r0",
    "ps2x_dsx_predicate": "ps_2_x\ndcl t0\ndsx r0, t0\nsetp_gt p0, r0, c0\n(p0) mov r0, c1\nmov oC0, r0",
}

VS2_CASES = {
    "vs2_oT_oD_fog": "vs_2_0\ndcl_position v0\ndcl_texcoord v1\nm4x4 oPos, v0, c0\nmov oT0, v1\nmov oD0, c4\nmov oFog, c5.x",
    "vs2_mova_relative": "vs_2_0\ndcl_position v0\ndcl_blendindices v1\nmova a0.x, v1.x\nm4x4 oPos, v0, c[a0.x + 0]",
    "vs2_loop": "vs_2_0\ndcl_position v0\nmov r0, c4\nloop aL, i0\nadd r0, r0, c[aL + 8]\nendloop\nm4x4 oPos, v0, c0\nmov oT0, r0",
}


def all_cases():
    cases = {}
    for name, body in PS3_BODIES.items():
        if "label" in body:
            # Subroutines follow the main program, so write the output before its ret.
            main, _, subs = body.partition("ret\n")
            cases["ps3_" + name] = PS3_HEAD + main + PS3_TAIL + "ret\n" + subs + "\n"
        else:
            cases["ps3_" + name] = PS3_HEAD + body + "\n" + PS3_TAIL
    for name, body in VS3_BODIES.items():
        cases["vs3_" + name[3:]] = VS3_HEAD + body + "\n" + VS3_TAIL
    cases.update(PS2_CASES)
    cases.update(VS2_CASES)
    return cases


if __name__ == "__main__":
    for name in all_cases():
        print(name)
