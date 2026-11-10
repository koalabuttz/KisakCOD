#!/usr/bin/env python3
"""Check the figures behind the PS Vita client's ``frame time`` log line on the host.

``src/gfx_gxm/gxm_frame_time.h`` keeps what the Legacy back end's calls took
since the last line (draws, of which about one in sixteen is timed, clears and
copies, scene starts and ends, waits for the GPU, the Present) and gives a
frame's mean of each. It is plain arithmetic, so this test builds
``tests/vita_gxm/frame_time_test.cpp`` with the host compiler and runs it:
the sums and the line's text, scene work and waits inside a timed call counted
once, a slow timed draw not scaled up, and the pick of timed draws (about one
in sixteen, and no fixed step over a frame that repeats its draws).

Usage:
    python3 scripts/tests/test_vita_frame_time.py [--cxx c++] [--timeout 120]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_gxm" / "frame_time_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-gxm-frame-time-") as work:
        binary = Path(work) / "frame_time_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the frame time test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: gxm frame time checks failed")
            return 1
    print("PASS: gxm frame time")
    return 0


if __name__ == "__main__":
    sys.exit(main())
