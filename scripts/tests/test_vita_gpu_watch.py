#!/usr/bin/env python3
"""Check on the host how the PS Vita client's back end times the GPU's work.

``src/gfx_gxm/gxm_gpu_watch.h`` is behind the ``gpu watch`` log line: with the
switch ``gpu-watch`` on, every scene carries a sceGxm notification for its
vertex job and one for its fragment job, and the back end notes when each
came. It is plain bookkeeping, so this test builds
``tests/vita_gxm/gpu_watch_test.cpp`` with the host compiler and runs it: one
scene, two scenes whose jobs overlap, jobs seen out of order, a frame's last
scene with the back end's own notification, more scenes than there are words
for, and the line itself.

Usage:
    python3 scripts/tests/test_vita_gpu_watch.py [--cxx c++] [--timeout 120]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_gxm" / "gpu_watch_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-gxm-gpu-watch-") as work:
        binary = Path(work) / "gpu_watch_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the GPU watch test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: gxm GPU watch checks failed")
            return 1
    print("PASS: gxm GPU watch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
