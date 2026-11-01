#!/usr/bin/env python3
"""Build and run tests/linux_compat/crt_shims_test.cpp for i386.

Covers the MSVC CRT shims in src/linux/compat.h that client sources call
directly (_itoa, the array strcpy_s, _time64/_localtime64), including the
fail-closed cases, which the test checks in forked children. Asset-free: one
test translation unit and the header, nothing else from the engine.

Usage: python3 scripts/tests/test_linux_crt_shims.py [--cxx clang++]
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "tests" / "linux_compat" / "crt_shims_test.cpp"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kisak-crt-shims-") as workdir:
        binary = Path(workdir) / "crt_shims_test"
        build = [options.cxx, "-m32", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "src"), str(TEST), "-o", str(binary)]
        print("+", " ".join(build), flush=True)
        if subprocess.run(build, timeout=options.timeout, check=False).returncode != 0:
            print("FAIL: crt_shims_test did not build")
            return 1
        completed = subprocess.run([str(binary)], timeout=options.timeout, capture_output=True, text=True,
                                   check=False)
        sys.stdout.write(completed.stdout)
        sys.stdout.write(completed.stderr)
        if completed.returncode != 0 or "PASS: CRT shims" not in completed.stdout:
            print(f"FAIL: crt_shims_test exited {completed.returncode}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
