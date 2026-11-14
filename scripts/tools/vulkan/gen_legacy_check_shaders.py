#!/usr/bin/env python3
"""Assemble the Legacy adapter check's D3D9 shaders into a C++ header.

KisakCOD-vulkan-legacy-check (src/gfx_vk/tools/vk_legacy_check.cpp) draws
through the Legacy adapter with small vertex and pixel shaders written in D3D9
assembly below. They are assembled to D3D9 bytecode with the shader spike's
assembler (scripts/tools/shader_spike/d3d9_asm.py) and checked in as words, so
building the check needs no shader tools. The assembler is plain Python, so
--check rebuilds the header in memory and fails when the checked-in one
differs (CI runs it).

    gen_legacy_check_shaders.py           # regenerate
    gen_legacy_check_shaders.py --check   # verify (CI)
"""

import argparse
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
HEADER = os.path.join(ROOT, "src", "gfx_vk", "tools", "vk_legacy_check_shaders.h")
sys.path.insert(0, os.path.join(ROOT, "scripts", "tools", "shader_spike"))

from d3d9_asm import assemble  # noqa: E402

# (C++ symbol, D3D9 assembly). Vertex positions are in clip space; c0 moves them.
SHADERS = [
    ("kVsBasic", """vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_color0 v2
dcl_position o0
dcl_texcoord0 o1
dcl_color0 o2
add o0, v0, c0
mov o1, v1
mov o2, v2
"""),
    ("kPsColor", """ps_3_0
dcl_color0 v0
mov oC0, v0
"""),
    ("kPsConstant", """ps_3_0
mov oC0, c0
"""),
    ("kPsTexture", """ps_3_0
dcl_texcoord0 v0.xy
dcl_2d s0
texld r0, v0, s0
mov oC0, r0
"""),
    # A lit surface in miniature: a colour map times the vertex colour plus a
    # light in c1, summed as the game's lit shaders sum their light terms.
    ("kPsLit", """ps_3_0
dcl_texcoord0 v0.xy
dcl_color0 v1
dcl_2d s0
texld r0, v0, s0
mul r0, r0, v1
add r0.xyz, r0, c1
mov oC0, r0
"""),
    # Texture coordinates as the cube direction (x, y, and z from the colour's red).
    ("kPsCube", """ps_3_0
dcl_texcoord0 v0.xy
dcl_color0 v1
dcl_cube s0
mov r1.xy, v0
mov r1.z, v1.x
texld r0, r1, s0
mov oC0, r0
"""),
    # Texture coordinates and the colour's red as w for the volume.
    ("kPsVolume", """ps_3_0
dcl_texcoord0 v0.xy
dcl_color0 v1
dcl_volume s0
mov r1.xy, v0
mov r1.z, v1.x
texld r0, r1, s0
mov oC0, r0
"""),
    # A depth texture sampled as a shadow map: the colour's red is the depth
    # it is compared with (D3D9's hardware shadow maps return 1 where the
    # reference is at or in front of the stored depth, 0 behind it).
    ("kPsShadow", """ps_3_0
dcl_texcoord0 v0.xy
dcl_color0 v1
dcl_2d s0
mov r1.xy, v0
mov r1.z, v1.x
texld r0, r1, s0
mov oC0, r0
"""),
    # Partial precision (_pp) as D3D9 hardware ran it, at full 32-bit
    # precision: c0.x squared is 90000, past 16-bit float's 65504, and times
    # c0.y gives 0.5. Computed in 16 bits it would overflow to infinity and
    # saturate to white.
    ("kPsPartialPrecision", """ps_3_0
mul_pp r0.x, c0.x, c0.x
mul_pp r0.x, r0.x, c0.y
mov r1, c0.w
mov_sat r1.xyz, r0.x
mov oC0, r1
"""),
    # The Modern lit shaders' check (vk_d3d9_check.cpp ModernLitShader): a
    # vertex shader writing what lm_hsm_sun_r0c0's pixel shader reads
    # (texcoord 0 colour and lightmap UVs, texcoord 1 normal and fog,
    # texcoord 4 shadow-map coordinate, colour 0), each but the colour from a
    # constant, and a stand-in for that pixel shader: it reads the same
    # samplers and constants up to c18 but shows black (times c1, kept 0), so
    # only the Modern shader drawn in its place shows light.
    ("kVsLmSun", """vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_color0 v2
dcl_position o0
dcl_texcoord0 o1
dcl_texcoord1 o2
dcl_texcoord4 o3
dcl_color0 o4
add o0, v0, c0
mov o1, c1
mov o2, c2
mov o3, c3
mov o4, v2
"""),
    ("kPsLmSunStandIn", """ps_3_0
dcl_texcoord0 v0
dcl_texcoord1 v1
dcl_texcoord4 v2
dcl_color0 v3
dcl_2d s0
dcl_2d s2
dcl_2d s3
dcl_2d s4
texld r0, v0, s0
texld r1, v0, s2
texld r2, v0, s3
texld r3, v2, s4
add r0, r0, r1
add r0, r0, r2
add r0, r0, r3
add r0, r0, v1
add r0, r0, v3
add r0, r0, c0
add r0, r0, c18
mul r0, r0, c1
mov oC0, r0
"""),
    # The same for the lm_hsm_sun_<mode>c0d0n0s0 family: the vertex shader also
    # writes the binormal (texcoord 2), tangent (texcoord 3) and eye-to-point
    # vector (texcoord 5) from c3, c4 and c6, and the stand-in reads the probe
    # cube at s1, the normal, specular and detail maps at s5-s7 and the
    # constants up to c19.
    ("kVsLmSunMaps", """vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_color0 v2
dcl_position o0
dcl_texcoord0 o1
dcl_texcoord1 o2
dcl_texcoord2 o3
dcl_texcoord3 o4
dcl_texcoord4 o5
dcl_texcoord5 o6
dcl_color0 o7
add o0, v0, c0
mov o1, c1
mov o2, c2
mov o3, c3
mov o4, c4
mov o5, c5
mov o6, c6
mov o7, v2
"""),
    ("kPsLmSunMapsStandIn", """ps_3_0
dcl_texcoord0 v0
dcl_texcoord1 v1
dcl_texcoord2 v2
dcl_texcoord3 v3
dcl_texcoord4 v4
dcl_texcoord5 v5
dcl_color0 v6
dcl_2d s0
dcl_cube s1
dcl_2d s2
dcl_2d s3
dcl_2d s4
dcl_2d s5
dcl_2d s6
dcl_2d s7
texld r0, v0, s0
texld r1, v5, s1
add r0, r0, r1
texld r1, v0, s2
add r0, r0, r1
texld r1, v0, s3
add r0, r0, r1
texld r1, v4, s4
add r0, r0, r1
texld r1, v0, s5
add r0, r0, r1
texld r1, v0, s6
add r0, r0, r1
texld r1, v0, s7
add r0, r0, r1
add r0, r0, v1
add r0, r0, v2
add r0, r0, v3
add r0, r0, v6
add r0, r0, c0
add r0, r0, c5
add r0, r0, c6
add r0, r0, c18
add r0, r0, c19
mul r0, r0, c1
mov oC0, r0
"""),
    # The same for the lp_hsm_sun_<mode>c0d0n0s0 family (models): the vertex
    # shader writes texcoords 0-6 from c1-c7 (texcoord 6 the lighting-volume
    # coordinate) and the colour, and the stand-in reads the colour map at s0,
    # the probe cube at s1, the lighting volume (3D) at s4, the shadow map at
    # s5, the normal, specular and detail maps at s6-s8 and the constants up
    # to c19, showing black.
    ("kVsLpSunMaps", """vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_color0 v2
dcl_position o0
dcl_texcoord0 o1
dcl_texcoord1 o2
dcl_texcoord2 o3
dcl_texcoord3 o4
dcl_texcoord4 o5
dcl_texcoord5 o6
dcl_texcoord6 o7
dcl_color0 o8
add o0, v0, c0
mov o1, c1
mov o2, c2
mov o3, c3
mov o4, c4
mov o5, c5
mov o6, c6
mov o7, c7
mov o8, v2
"""),
    ("kPsLpSunMapsStandIn", """ps_3_0
dcl_texcoord0 v0
dcl_texcoord1 v1
dcl_texcoord2 v2
dcl_texcoord3 v3
dcl_texcoord4 v4
dcl_texcoord5 v5
dcl_texcoord6 v6
dcl_color0 v7
dcl_2d s0
dcl_cube s1
dcl_volume s4
dcl_2d s5
dcl_2d s6
dcl_2d s7
dcl_2d s8
texld r0, v0, s0
texld r1, v5, s1
add r0, r0, r1
texld r1, v6, s4
add r0, r0, r1
texld r1, v4, s5
add r0, r0, r1
texld r1, v0, s6
add r0, r0, r1
texld r1, v0, s7
add r0, r0, r1
texld r1, v0, s8
add r0, r0, r1
add r0, r0, v1
add r0, r0, v2
add r0, r0, v3
add r0, r0, v7
add r0, r0, c0
add r0, r0, c5
add r0, r0, c6
add r0, r0, c7
add r0, r0, c18
add r0, r0, c19
mul r0, r0, c1
mov oC0, r0
"""),
    # Shader model 2: the same path through fixed output registers.
    ("kVs20", """vs_2_0
dcl_position v0
dcl_texcoord0 v1
dcl_color0 v2
add oPos, v0, c0
mov oT0, v1
mov oD0, v2
"""),
    ("kPs20", """ps_2_0
dcl t0.xy
dcl v0
dcl_2d s0
texld r0, t0, s0
mul r0, r0, v0
mov oC0, r0
"""),
    # As the engine's world shaders place a vertex: the eye-relative world
    # matrix (c4-c7) first, then the view-projection (c0-c3), each a column
    # of D3D's row-vector matrix (vk_d3d9_check.cpp ModernVelocity's brush run).
    ("kVsWorld", """vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_color0 v2
dcl_position o0
dcl_texcoord0 o1
dcl_color0 o2
dp4 r0.x, v0, c4
dp4 r0.y, v0, c5
dp4 r0.z, v0, c6
dp4 r0.w, v0, c7
dp4 o0.x, r0, c0
dp4 o0.y, r0, c1
dp4 o0.z, r0, c2
dp4 o0.w, r0, c3
mov o1, v1
mov o2, v2
"""),
]


def render():
    lines = [
        "#pragma once",
        "",
        "// Generated by scripts/tools/vulkan/gen_legacy_check_shaders.py from the D3D9",
        "// assembly in that script. Do not edit; run the script after changing it.",
        "",
        "#include <cstdint>",
        "",
        "namespace vk_legacy_check_shaders",
        "{",
    ]
    for symbol, source in SHADERS:
        code = assemble(source)
        words = struct.unpack("<%dI" % (len(code) // 4), code)
        lines.append("// " + source.splitlines()[0])
        lines.append("inline constexpr uint32_t %s[] = {" % symbol)
        for i in range(0, len(words), 6):
            lines.append("    " + " ".join("0x%08xu," % w for w in words[i:i + 6]))
        lines.append("};")
    lines.append("} // namespace vk_legacy_check_shaders")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if the header is out of date")
    args = parser.parse_args()
    text = render()
    if args.check:
        try:
            with open(HEADER) as handle:
                current = handle.read()
        except FileNotFoundError:
            current = ""
        if current != text:
            print("gen_legacy_check_shaders.py: %s is out of date; run the script" % os.path.relpath(HEADER, ROOT))
            return 1
        print("gen_legacy_check_shaders.py: %s is up to date (%d shaders)" % (os.path.relpath(HEADER, ROOT),
                                                                              len(SHADERS)))
        return 0
    with open(HEADER, "w", newline="\n") as handle:
        handle.write(text)
    print("wrote %s" % os.path.relpath(HEADER, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
