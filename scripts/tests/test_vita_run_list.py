#!/usr/bin/env python3
"""Check the PS Vita build's list of starts on the host.

``src/vita/vita_run_list.h`` reads the list a console run puts on the card:
one start a line, each with its name, its marker files and its command line
(docs/vita.md, "A list of starts"). It makes no engine call and no Vita call,
so this builds ``tests/vita_client/run_list_test.cpp`` with the host compiler
(ASan and UBSan on) and runs it: what a list yields, and that a bad name,
marker or line refuses the whole list with the line's number.

Usage:
    python3 scripts/tests/test_vita_run_list.py [--cxx c++] [--timeout 300]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_client" / "run_list_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-run-list-") as work:
        binary = Path(work) / "run_list_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the run list test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: vita run list checks failed")
            return 1
    print("PASS: vita run list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
