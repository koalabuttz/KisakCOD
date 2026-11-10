#!/usr/bin/env python3
"""Check the PS Vita build's run plan on the host.

``src/vita/vita_run_plan.h`` decides when a run that nobody watches prints its
figures, sends its timed console commands and quits (docs/vita.md, "Runs nobody
watches"): ``vita_statsEvery``, ``vita_quitAfter``, ``vita_quitAfterMenu``,
``vita_quitAfterMap``, ``vita_atMenu`` and ``vita_atMap``. It makes no engine
call and no Vita call, so this test builds ``tests/vita_client/run_plan_test.cpp``
with the host compiler and runs it: the timed-command parser, the default
once-a-minute figures, each quit timer with its last figures and its wait for
the renderer's and the sound's lines, and a run at an emulator's two frames a
second.

Built with AddressSanitizer and UBSan.

Usage:
    python3 scripts/tests/test_vita_run_plan.py [--cxx c++] [--timeout 300]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_client" / "run_plan_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-run-plan-") as work:
        binary = Path(work) / "run_plan_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the run plan test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: vita run plan checks failed")
            return 1
    print("PASS: vita run plan")
    return 0


if __name__ == "__main__":
    sys.exit(main())
