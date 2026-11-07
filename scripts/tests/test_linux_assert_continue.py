#!/usr/bin/env python3
"""Build and run tests/linux_compat/assert_continue_test.cpp for i386.

Links src/linux/sys_assert.cpp, the native assertion handler, with the test's
stand-in Com_PrintError and checks that assertions stay fatal by default and
that the opt-in KISAK_ASSERT_CONTINUE=1 survey mode reports each failing site
at powers of two and returns. Asset-free: the handler's unit and the test,
nothing else from the engine.

Usage: python3 scripts/tests/test_linux_assert_continue.py [--cxx clang++]
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "tests" / "linux_compat" / "assert_continue_test.cpp"
HANDLER = ROOT / "src" / "linux" / "sys_assert.cpp"
# The engine headers sys_assert.cpp includes need the game's dialect.
ENGINE_FLAGS = ["-fms-extensions", "-Wno-c++11-narrowing", "-Wno-invalid-token-paste",
                "-Wno-ignored-attributes", "-DKISAK_SP", "-DRELEASE_ASSERTS"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kisak-assert-continue-") as workdir:
        binary = Path(workdir) / "assert_continue_test"
        build = [options.cxx, "-m32", "-std=c++20", *ENGINE_FLAGS, "-I", str(ROOT / "src"),
                 "-I", str(ROOT / "deps"), str(HANDLER), str(TEST), "-o", str(binary)]
        print("+", " ".join(build), flush=True)
        if subprocess.run(build, timeout=options.timeout, check=False).returncode != 0:
            print("FAIL: assert_continue_test did not build")
            return 1
        completed = subprocess.run([str(binary)], timeout=options.timeout, capture_output=True, text=True,
                                   check=False)
        sys.stdout.write(completed.stdout)
        sys.stdout.write(completed.stderr)
        if completed.returncode != 0 or "PASS: assert continue mode" not in completed.stdout:
            print(f"FAIL: assert_continue_test exited {completed.returncode}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
