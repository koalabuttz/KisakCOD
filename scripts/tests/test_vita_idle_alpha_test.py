#!/usr/bin/env python3
"""Check on the host which alpha tests the PS Vita client's back end drops.

``src/gfx_gxm/gxm_idle_alpha_test.h`` says, from the D3D9 render state alone,
when a draw shows the same picture without its alpha test: the test rejects
only pixels of alpha 0, the blend adds nothing for such a pixel, and the draw
writes neither depth nor stencil. The back end then draws with the fragment
program that has no ``discard``. This test builds
``tests/vita_gxm/idle_alpha_test.cpp`` with the host compiler and runs it: the
game's known states, and every state the header calls idle drawn through
D3D9's blend with and without its test, for every source alpha.

Usage:
    python3 scripts/tests/test_vita_idle_alpha_test.py [--cxx c++] [--timeout 120]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_gxm" / "idle_alpha_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-gxm-idle-alpha-") as work:
        binary = Path(work) / "idle_alpha_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the idle alpha test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: gxm idle alpha test checks failed")
            return 1
    print("PASS: gxm idle alpha test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
