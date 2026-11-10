#!/usr/bin/env python3
"""Check the PS Vita client's control mapping on the host.

``src/vita/client/input/vita_input_map.cpp`` turns the Vita's buttons, sticks
and touch panels into engine keys and mouse motion (docs/vita-client.md,
"Controls"). It makes no engine call and no Vita call, so this test builds it
with the host compiler and runs ``tests/vita_client/input_map_test.cpp``, which
feeds it made-up readings and reads the keys and the motion that come out: the
default layout, the move stick's four keys, the look stick's speed and curve,
the menu's cursor, clicks, arrows and wheel, the touch areas, what is let go
when a menu opens or closes, and the controls menu's key capture.

Built with AddressSanitizer and UBSan.

Usage:
    python3 scripts/tests/test_vita_input.py [--cxx c++] [--timeout 300]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "src" / "vita" / "client" / "input" / "vita_input_map.cpp",
    ROOT / "tests" / "vita_client" / "input_map_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-input-") as work:
        binary = Path(work) / "input_map_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the input test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: vita input checks failed")
            return 1
    print("PASS: vita input")
    return 0


if __name__ == "__main__":
    sys.exit(main())
