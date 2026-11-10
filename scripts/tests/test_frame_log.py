#!/usr/bin/env python3
"""Check the "framelog" command's recording and arithmetic on the host.

``framelog <seconds> [label]`` records the time from each Com_Frame to the next
and prints one line with the frame rate, the mean, median, 95th and 99th
percentile and longest frame time and the count of frames over 50 ms, so a
frame-rate comparison needs no profiler capture (docs/runs.md).
``src/qcommon/com_frame_log.h`` holds the recording and the arithmetic with no
engine call. This test builds ``tests/frame_log/frame_log_test.cpp`` with the
host compiler and runs it: the percentile rule, the summary, the exact log
line, and a recording on a made-up clock.

Built with AddressSanitizer and UBSan.

Usage:
    python3 scripts/tests/test_frame_log.py [--cxx c++] [--timeout 300]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "frame_log" / "frame_log_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-frame-log-") as work:
        binary = Path(work) / "frame_log_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the frame log test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: frame log checks failed")
            return 1
    print("PASS: frame log")
    return 0


if __name__ == "__main__":
    sys.exit(main())
