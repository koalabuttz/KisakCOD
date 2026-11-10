#!/usr/bin/env python3
"""Check the in-game A/B measuring commands' logic (docs/perf-ab.md).

``perf_ab`` and ``perf_sample`` (``src/linux/sys_perf.cpp``) measure the game
from inside one launch: windows of frames that alternate between two sets of
console commands, with CPU time, time spent waiting for the GPU and, on a
laptop, battery power and GPU frequency. A run on a real machine costs a
laptop's battery and its place in a queue, so what the commands compute has to
be right before one is spent.

The parts that hold the logic use no engine types and are compiled here as
they are:

  * ``src/linux/sys_perf_core.h``: window order (A B B A, so a steady drift
    falls on both variants alike), settle time, frame statistics, counter
    differences across a wrap, the lines printed;
  * ``src/linux/sys_perf_readings.h``: the files Linux reports power,
    frequency, temperature and per-thread CPU time in, read from a made-up tree
    of the same shape.

``tests/linux_perf/perf_ab_test.cpp`` drives both with a made-up clock, a
made-up game whose speed depends on the variant, and made-up files. No engine,
no game data, no GPU, and nothing read from the real machine except this
test's own entry in /proc.

Usage:
    python3 scripts/tests/test_linux_perf_ab.py [--cxx clang++] [--m32] [--timeout 120]
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "linux_perf" / "perf_ab_test.cpp"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default="clang++", help="C++ compiler (default: clang++)")
    parser.add_argument("--m32", action="store_true", help="build the test as a 32-bit program")
    parser.add_argument("--timeout", type=int, default=120, help="seconds allowed per step")
    options = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisakcod_perf_ab_") as scratch:
        binary = Path(scratch) / "perf_ab_test"
        compile_command = [
            options.cxx,
            *(["-m32"] if options.m32 else []),
            "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "src"),
            str(SOURCE),
            "-o", str(binary),
        ]
        compiled = subprocess.run(compile_command, capture_output=True, text=True, timeout=options.timeout)
        if compiled.returncode != 0:
            sys.stderr.write(compiled.stdout + compiled.stderr)
            print("FAIL: the perf_ab test did not compile")
            return 1
        tree = Path(scratch) / "tree"
        tree.mkdir()
        ran = subprocess.run([str(binary), str(tree)], capture_output=True, text=True, timeout=options.timeout)
        sys.stdout.write(ran.stdout)
        sys.stderr.write(ran.stderr)
        if ran.returncode != 0:
            print("FAIL: the perf_ab test reported failures")
            return 1
    print("PASS: perf_ab window order, statistics and machine readings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
