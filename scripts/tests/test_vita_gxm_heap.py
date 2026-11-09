#!/usr/bin/env python3
"""Check the PS Vita GXM back end's GPU memory heap on the host.

``src/gfx_gxm/gxm_heap.cpp`` hands out pieces of the large GPU memory blocks
the Vita client reserves at start-up (textures, surfaces, vertex data). It is
plain offset arithmetic, so this test builds it with the host compiler and
runs ``tests/vita_gxm/heap_test.cpp``: alignment, merging of freed ranges,
running out of space, the top of the 32-bit range, and a seeded random
workload checked against a model.

Usage:
    python3 scripts/tests/test_vita_gxm_heap.py [--cxx c++] [--timeout 120]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "src" / "gfx_gxm" / "gxm_heap.cpp",
    ROOT / "tests" / "vita_gxm" / "heap_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-gxm-heap-") as work:
        binary = Path(work) / "heap_test"
        # -m32 is not needed: the heap uses fixed-width types only.
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the heap test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: gxm heap checks failed")
            return 1
    print("PASS: gxm heap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
