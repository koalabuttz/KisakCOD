#!/usr/bin/env python3
"""Compile the Vulkan code's own GLSL shaders into C++ headers of SPIR-V words.

Three sets: the probe's (src/gfx_vk/tools/shaders), the Modern path's
(src/gfx_vk/modern/shaders) and the Legacy adapter's present passes
(src/gfx_vk/legacy/shaders). All are built on platforms without a shader
compiler (MSVC with only the Vulkan headers), so the SPIR-V is generated here
and checked in next to its GLSL source, like the other generated tables in
this repository. The header
records a SHA-256 of each source; --check fails when a source changed without
the header being regenerated, and runs spirv-val on the embedded words when it
is installed. --check needs no compiler, so it does not depend on which
glslang version the CI image has.

    gen_spirv_header.py           # regenerate (needs glslangValidator)
    gen_spirv_header.py --check   # verify (CI)
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


class ShaderSet:
    def __init__(self, shader_dir, header, namespace, shaders):
        self.shader_dir = os.path.join(ROOT, *shader_dir.split("/"))
        self.header = os.path.join(ROOT, *header.split("/"))
        self.namespace = namespace
        # (source file, C++ symbol) or (source file, C++ symbol, defines):
        # defines ("NAME=VALUE" strings) are passed to the compiler as -D and
        # are part of the recorded hash, so one source can make several
        # shaders.
        self.shaders = [entry if len(entry) == 3 else (entry[0], entry[1], ()) for entry in shaders]


# The lm_[hsm_]sun world shader's variants (vk_legacy_modern_lit.cpp): with or
# without the shadow map, times the material maps the game's shaders come
# with (D detail, N normal, S specular).
LM_SUN_VARIANTS = [
    ("lm_sun.frag", f"kLmSun{'Shadow' if shadow else ''}{maps}Frag",
     (f"LM_SUN_SHADOW={shadow}", f"LM_SUN_DETAIL={int('D' in maps)}", f"LM_SUN_NORMAL={int('N' in maps)}",
      f"LM_SUN_SPECULAR={int('S' in maps)}"))
    for shadow in (0, 1)
    for maps in ("", "N", "S", "NS", "D", "DN", "DS", "DNS")
]
LP_SUN_VARIANTS = [
    ("lp_sun.frag", f"kLpSun{'Shadow' if shadow else ''}{maps}Frag",
     (f"LP_SUN_SHADOW={shadow}", f"LP_SUN_DETAIL={int('D' in maps)}", f"LP_SUN_NORMAL={int('N' in maps)}",
      f"LP_SUN_SPECULAR={int('S' in maps)}"))
    for shadow in (0, 1)
    for maps in ("", "N", "S", "NS", "D", "DN", "DS", "DNS")
]
# The same sources without the sun (lm_<mode>c0... and lp_[i_]<mode>c0...:
# interiors), for each set of maps.
NO_SUN_VARIANTS = [
    (f"{family}_sun.frag", f"k{family.capitalize()}NoSun{maps}Frag",
     (f"{family.upper()}_SUN_LIGHT=0", f"{family.upper()}_SUN_DETAIL={int('D' in maps)}",
      f"{family.upper()}_SUN_NORMAL={int('N' in maps)}", f"{family.upper()}_SUN_SPECULAR={int('S' in maps)}"))
    for family in ("lm", "lp")
    for maps in ("", "N", "S", "NS", "D", "DN", "DS", "DNS")
]
# lp_[i_]amb[_<light>]_t0c0: the model shader's ambient-only build (lit shader
# facts 6).
AMBIENT_VARIANTS = [("lp_sun.frag", "kLpAmbFrag", ("LP_SUN_AMBIENT=1",))]
# The same sources with a spot (2) or omni (3) light in place of the sun
# (lm_/lp_[hsm_][i_]spot|omni_<mode>c0...: lit shader facts 6 and 7), with or
# without its shadow map; facts 7 did not read the shadowed ones with a
# detail map, so those are not built.
LIGHT_VARIANTS = [
    (f"{family}_sun.frag", f"k{family.capitalize()}{light}{'Shadow' if shadow else ''}{maps}Frag",
     (f"{family.upper()}_SUN_LIGHT={value}", f"{family.upper()}_SUN_SHADOW={shadow}",
      f"{family.upper()}_SUN_DETAIL={int('D' in maps)}", f"{family.upper()}_SUN_NORMAL={int('N' in maps)}",
      f"{family.upper()}_SUN_SPECULAR={int('S' in maps)}"))
    for family in ("lm", "lp")
    for light, value in (("Spot", 2), ("Omni", 3))
    for shadow in (0, 1)
    for maps in ("", "N", "S", "NS", "D", "DN", "DS", "DNS")
    if not (shadow and "D" in maps)
]


SETS = [
    ShaderSet(
        "src/gfx_vk/tools/shaders",
        "src/gfx_vk/tools/vk_probe_shaders.h",
        "vk_probe_shaders",
        [
            ("probe_draw.vert", "kProbeDrawVert"),
            ("probe_draw.frag", "kProbeDrawFrag"),
        ],
    ),
    ShaderSet(
        "src/gfx_vk/modern/shaders",
        "src/gfx_vk/modern/vk_modern_shaders.h",
        "gfx_vk::modern::shaders",
        [
            ("fullscreen.vert", "kFullscreenVert"),
            ("taa.frag", "kTaaFrag"),
            ("tonemap.frag", "kTonemapFrag"),
            ("bloom_down.frag", "kBloomDownFrag"),
            ("bloom_up.frag", "kBloomUpFrag"),
            ("velocity.vert", "kVelocityVert"),
            ("velocity.frag", "kVelocityFrag"),
            ("debug_velocity.frag", "kDebugVelocityFrag"),
            ("exposure_measure.frag", "kExposureMeasureFrag"),
            ("exposure_reduce.frag", "kExposureReduceFrag"),
            ("exposure_adapt.frag", "kExposureAdaptFrag"),
            ("normals.frag", "kNormalsFrag"),
            ("debug_normals.frag", "kDebugNormalsFrag"),
            ("gtao.frag", "kGtaoFrag"),
            ("ao_blur.frag", "kAoBlurFrag"),
            ("debug_ao.frag", "kDebugAoFrag"),
            ("reflections.frag", "kReflectionsFrag"),
            ("debug_reflections.frag", "kDebugReflectionsFrag"),
        ],
    ),
    ShaderSet(
        "src/gfx_vk/legacy/shaders",
        "src/gfx_vk/legacy/vk_legacy_shaders.h",
        "gfx_vk::legacy::shaders",
        [
            ("fullscreen.vert", "kFullscreenVert"),
            ("gamma.frag", "kGammaFrag"),
        ] + LM_SUN_VARIANTS + LP_SUN_VARIANTS + NO_SUN_VARIANTS + AMBIENT_VARIANTS + LIGHT_VARIANTS,
    ),
]

TARGET_ENV = "vulkan1.3"


def source_hash(shader_set, name, defines=()):
    with open(os.path.join(shader_set.shader_dir, name), "rb") as handle:
        digest = hashlib.sha256(handle.read())
    for define in defines:
        digest.update(b"\0-D" + define.encode("utf-8"))
    return digest.hexdigest()


def compile_shader(shader_set, name, out_path, defines=()):
    compiler = shutil.which("glslangValidator")
    if not compiler:
        sys.exit("gen_spirv_header.py: glslangValidator is not installed (apt install glslang-tools)")
    subprocess.run(
        [compiler, "-V", "--target-env", TARGET_ENV, *("-D" + define for define in defines), "-o", out_path,
         os.path.join(shader_set.shader_dir, name)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    with open(out_path, "rb") as handle:
        data = handle.read()
    if len(data) % 4:
        sys.exit(f"gen_spirv_header.py: {name}: SPIR-V size {len(data)} is not a whole number of words")
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def render(shader_set, entries):
    source_dir = os.path.relpath(shader_set.shader_dir, ROOT).replace(os.sep, "/")
    lines = [
        "#pragma once",
        "",
        "// Generated by scripts/tools/vulkan/gen_spirv_header.py from",
        f"// {source_dir}; do not edit. Regenerate after changing a shader.",
        "",
        "#include <cstdint>",
        "",
        f"namespace {shader_set.namespace}",
        "{",
    ]
    for name, symbol, digest, words in entries:
        lines.append(f"// {name} sha256 {digest}")
        lines.append(f"inline constexpr uint32_t {symbol}[{len(words)}] = {{")
        for i in range(0, len(words), 8):
            chunk = ", ".join(f"0x{word:08x}" for word in words[i:i + 8])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    lines.append(f"}} // namespace {shader_set.namespace}")
    lines.append("")
    return "\n".join(lines)


def parse_header(text):
    """Returns {symbol: (source name, digest, words)}."""
    result = {}
    pattern = re.compile(
        r"// (\S+) sha256 ([0-9a-f]{64})\ninline constexpr uint32_t (\w+)\[(\d+)\] = \{\n(.*?)\n\};", re.S
    )
    for match in pattern.finditer(text):
        name, digest, symbol, count, body = match.groups()
        words = [int(word, 16) for word in re.findall(r"0x([0-9a-f]{8})", body)]
        if len(words) != int(count):
            sys.exit(f"gen_spirv_header.py: {symbol}: {len(words)} words, declared {count}")
        result[symbol] = (name, digest, words)
    return result


def check():
    failures = 0
    validator = shutil.which("spirv-val")
    for shader_set in SETS:
        failures += check_set(shader_set, validator)
    if not validator:
        print("note: spirv-val is not installed; the words were not validated")
    return 1 if failures else 0


def check_set(shader_set, validator):
    if not os.path.exists(shader_set.header):
        print(f"FAIL {os.path.relpath(shader_set.header, ROOT)} is missing; run gen_spirv_header.py")
        return 1
    with open(shader_set.header, encoding="utf-8") as handle:
        parsed = parse_header(handle.read())
    failures = 0
    for name, symbol, defines in shader_set.shaders:
        if symbol not in parsed:
            print(f"FAIL {symbol}: missing from {os.path.relpath(shader_set.header, ROOT)}")
            failures += 1
            continue
        recorded_name, digest, words = parsed[symbol]
        if recorded_name != name or digest != source_hash(shader_set, name, defines):
            print(f"FAIL {symbol}: {name} changed since the header was generated; run gen_spirv_header.py")
            failures += 1
            continue
        if not words or words[0] != 0x07230203:
            print(f"FAIL {symbol}: not SPIR-V (magic {words[0] if words else 0:08x})")
            failures += 1
            continue
        if validator:
            with tempfile.TemporaryDirectory() as scratch:
                path = os.path.join(scratch, symbol + ".spv")
                with open(path, "wb") as handle:
                    handle.write(b"".join(word.to_bytes(4, "little") for word in words))
                run = subprocess.run(
                    [validator, "--target-env", TARGET_ENV, path], capture_output=True, text=True
                )
                if run.returncode:
                    print(f"FAIL {symbol}: spirv-val: {run.stdout.strip()} {run.stderr.strip()}")
                    failures += 1
                    continue
        print(f"ok   {symbol} ({name}, {len(words)} words{', spirv-val clean' if validator else ''})")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify the header instead of writing it")
    args = parser.parse_args()
    if args.check:
        return check()
    for shader_set in SETS:
        entries = []
        with tempfile.TemporaryDirectory() as scratch:
            for name, symbol, defines in shader_set.shaders:
                words = compile_shader(shader_set, name, os.path.join(scratch, symbol + ".spv"), defines)
                entries.append((name, symbol, source_hash(shader_set, name, defines), words))
        with open(shader_set.header, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(render(shader_set, entries))
        print(f"wrote {os.path.relpath(shader_set.header, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
