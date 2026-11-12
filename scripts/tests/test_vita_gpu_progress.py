#!/usr/bin/env python3
"""Check on the host which frames the PS Vita client's back end takes as drawn.

``src/gfx_gxm/gxm_gpu_progress.h`` lets the CPU fill a frame while the GPU
still draws the one before: the last scene of every frame carries a sceGxm
notification, and the back end reads from them which frames are done, so that
it overwrites nothing the GPU still reads and waits only for the frame before
last. It is plain bookkeeping, so this test builds
``tests/vita_gxm/gpu_progress_test.cpp`` with the host compiler and runs it:
one frame ahead, a frame without a notification, a wait for the whole GPU,
the region's words used again.

Usage:
    python3 scripts/tests/test_vita_gpu_progress.py [--cxx c++] [--timeout 120]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_gxm" / "gpu_progress_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-gxm-gpu-progress-") as work:
        binary = Path(work) / "gpu_progress_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the GPU progress test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: gxm GPU progress checks failed")
            return 1
    print("PASS: gxm GPU progress")
    return 0


if __name__ == "__main__":
    sys.exit(main())
