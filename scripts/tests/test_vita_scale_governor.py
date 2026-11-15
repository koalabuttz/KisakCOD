#!/usr/bin/env python3
"""Check the GXM back end's 3D picture size governor on the host.

``src/gfx_gxm/gxm_scale_governor.h`` picks, from each frame's time and its
waits for the GPU, the size the Vita draws its 3D picture at. It makes no
engine call and no Vita call, so this builds
``tests/vita_gxm/scale_governor_test.cpp`` with the host compiler (ASan and
UBSan on) and runs it on a model of the console with console run BV's times.

Usage:
    python3 scripts/tests/test_vita_scale_governor.py [--cxx c++] [--timeout 300]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_gxm" / "scale_governor_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-scale-governor-") as work:
        binary = Path(work) / "scale_governor_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the scale governor test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: vita scale governor checks failed")
            return 1
    print("PASS: vita scale governor")
    return 0


if __name__ == "__main__":
    sys.exit(main())
