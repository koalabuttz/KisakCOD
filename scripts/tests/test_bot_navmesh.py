#!/usr/bin/env python3
"""Check KisakCOD's bot navigation mesh on the host.

``src/bots/bot_navmesh.cpp`` builds where a player can walk on a map with
Recast and finds ways with Detour (``deps/recastnavigation``), for the
multiplayer bots (docs/bots.md). It makes no engine call, so this test builds
``tests/bots/bot_navmesh_test.cpp`` with the Recast and Detour sources on the
host and runs it on made-up rooms: an open floor, a door a player fits
through and one too narrow, a step and a ledge, a walkable and a too-steep
ramp, a floor that faces down, two rooms with no way between (random goals
stay in the bot's room) and a map-sized yard of tiles; and the cache blob
refuses anything truncated, padded or of another version.

Built 64-bit with AddressSanitizer and UBSan when available, and 32-bit
(``--m32`` makes the 32-bit build a requirement), as the i386 game builds it.

It also builds ``tests/bots/brush_faces_test.cpp`` with the engine's
``cm_showcollision.cpp``, which turns each brush into the faces the mesh is
built from, with clang and AddressSanitizer on both widths: a box and a
wedge give exactly their faces, without overrunning the corner buffer or
letting a neighbouring brush's side admit corners outside the brush (PC bot
brain run 3 crashed there). The decompiled unit indexes plane arrays in ways
UBSan reports, so this build uses AddressSanitizer alone.

Usage:
    python3 scripts/tests/test_bot_navmesh.py [--cxx c++] [--engine-cxx clang++] [--m32]
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [ROOT / "tests" / "bots" / "bot_navmesh_test.cpp", ROOT / "src" / "bots" / "bot_navmesh.cpp"]
SOURCES += sorted((ROOT / "deps" / "recastnavigation" / "Recast").glob("*.cpp"))
SOURCES += sorted((ROOT / "deps" / "recastnavigation" / "Detour").glob("*.cpp"))
# Detour clears its tile structs with memset and GCC suspects two arrays it
# fills in a loop of uninitialised use; the rest of the warnings stay errors.
BASE_FLAGS = ["-std=c++20", "-g", "-O1", f"-I{ROOT / 'src'}", f"-I{ROOT / 'deps'}", "-Wall", "-Wextra", "-Werror",
              "-Wno-unknown-warning-option", "-Wno-class-memaccess", "-Wno-maybe-uninitialized"]


BRUSH_SOURCES = [ROOT / "tests" / "bots" / "brush_faces_test.cpp", ROOT / "src" / "qcommon" / "cm_showcollision.cpp",
                 ROOT / "src" / "universal" / "com_math.cpp"]
# Engine units: the defines and dialect of the dedicated server's build.
BRUSH_FLAGS = ["-std=gnu++20", "-g", "-O1", "-fms-extensions", "-w", "-DKISAK_MP", "-DDEDICATED", "-DKISAK_DEDICATED",
               "-D_DEBUG", f"-I{ROOT / 'src'}", f"-I{ROOT / 'deps'}", "-fsanitize=address"]


def build_and_run(cxx, out, flags, label, base=None, sources=None):
    base = BASE_FLAGS if base is None else base
    sources = SOURCES if sources is None else sources
    build = subprocess.run([cxx, *base, *flags, *map(str, sources), "-o", str(out)],
                           capture_output=True, text=True)
    if build.returncode != 0:
        return None, build.stdout + build.stderr
    run = subprocess.run([str(out)], capture_output=True, text=True, timeout=300)
    print(f"--- {label} ---")
    print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    return run.returncode, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--engine-cxx", default="clang++", help="compiler for the engine's brush faces unit")
    parser.add_argument("--m32", action="store_true", help="fail when the 32-bit build cannot be made")
    options = parser.parse_args()

    failed = False
    with tempfile.TemporaryDirectory(prefix="kisak-bot-navmesh-") as tmp:
        tmp = Path(tmp)
        code, log = build_and_run(options.cxx, tmp / "sanitized",
                                  ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
                                  "64-bit, sanitized")
        if code is None:
            code, log2 = build_and_run(options.cxx, tmp / "plain", [], "64-bit")
            if code is None:
                print(log + log2, file=sys.stderr)
                print("test_bot_navmesh: the host build failed", file=sys.stderr)
                return 1
        failed |= code != 0

        code, log = build_and_run(options.cxx, tmp / "i386", ["-m32", "-msse2", "-mfpmath=sse"], "32-bit")
        if code is None:
            if options.m32:
                print(log, file=sys.stderr)
                print("test_bot_navmesh: the 32-bit build failed (--m32)", file=sys.stderr)
                return 1
            print("test_bot_navmesh: no 32-bit toolchain, 32-bit run skipped")
        else:
            failed |= code != 0

        for flags, label in (([], "brush faces, 64-bit, AddressSanitizer"),
                             (["-m32", "-msse2", "-mfpmath=sse"], "brush faces, 32-bit, AddressSanitizer")):
            code, log = build_and_run(options.engine_cxx, tmp / "brush", flags, label, BRUSH_FLAGS, BRUSH_SOURCES)
            if code is None:
                if flags and not options.m32:
                    print("test_bot_navmesh: no 32-bit toolchain, 32-bit brush faces run skipped")
                    continue
                print(log, file=sys.stderr)
                print(f"test_bot_navmesh: the {label} build failed", file=sys.stderr)
                return 1
            failed |= code != 0

    if failed:
        print("test_bot_navmesh: FAILED")
        return 1
    print("test_bot_navmesh: the bot navigation mesh holds on the host")
    return 0


if __name__ == "__main__":
    sys.exit(main())
