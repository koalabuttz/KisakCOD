#!/usr/bin/env python3
"""Check the static model cache's size arithmetic at every size.

``src/gfx_d3d/r_staticmodelcache_sizes.h`` derives the cache's sizes and the
places in it from one number, the trees a section has, so that the PS Vita
can run a smaller cache than the retail game. It is plain arithmetic, so this test
builds ``tests/vita_gxm/smodel_cache_sizes_test.cpp`` with the host compiler
once for every size the header allows (2 to 128 trees a section) and once
each for the two defaults (128 trees; 32 with ``KISAK_GFX_GXM``), and runs
each: every leaf of every tree must be one place to the cache's handle, to
the vertex buffer and to a draw's stream offset and 16-bit index, and the
retail size must give the retail game's own expressions.

Usage:
    python3 scripts/tests/test_static_model_cache_sizes.py [--cxx c++] [--timeout 120]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "vita_gxm" / "smodel_cache_sizes_test.cpp"

# (name, the compiler's defines)
VARIANTS = [(f"{1 << bits} trees a section", [f"-DR_SMC_SECTION_TREE_BITS={bits}"]) for bits in range(1, 8)]
VARIANTS += [
    ("the default", ["-DEXPECT_BITS=7"]),
    ("the default of the GXM back end", ["-DKISAK_GFX_GXM", "-DEXPECT_BITS=6"]),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    failed = 0
    with tempfile.TemporaryDirectory(prefix="kisak-smc-sizes-") as work:
        for number, (name, defines) in enumerate(VARIANTS):
            binary = Path(work) / f"sizes_test_{number}"
            command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       *defines, "-I", str(ROOT / "src"), str(SOURCE), "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            if build.returncode:
                sys.stdout.write(build.stdout + build.stderr)
                print(f"FAIL: {name}: the test did not build")
                failed += 1
                continue
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
            sys.stdout.write(f"{name}: {run.stdout}{run.stderr}")
            if run.returncode:
                print(f"FAIL: {name}")
                failed += 1
    if failed:
        print(f"FAIL: static model cache sizes, {failed} of {len(VARIANTS)}")
        return 1
    print(f"PASS: static model cache sizes, {len(VARIANTS)} builds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
