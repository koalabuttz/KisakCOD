#!/usr/bin/env python3
"""Check KisakCOD's bot view motor on the host.

``src/bots/bot_motor.h`` is the "hand" of the multiplayer bots (docs/bots.md):
it turns what a bot wants to look at into the user command angles a player's
client would send, at a human pace. It makes no engine call, so this test
builds ``tests/bots/bot_motor_test.cpp`` with the host compiler and runs it:
commands round-trip to the view Pmove computes from them, turns take the short
way, keep their top speed, never overshoot, settle in a human time and do not
depend on the frame rate, and each bot's random choices replay from its seed.

Built 64-bit with AddressSanitizer and UBSan when available, and 32-bit
(``--m32`` makes the 32-bit build a requirement), as the i386 game builds it.

Usage:
    python3 scripts/tests/test_bot_motor.py [--cxx c++] [--m32]
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "bots" / "bot_motor_test.cpp"
BASE_FLAGS = ["-std=c++20", "-g", "-O1", f"-I{ROOT / 'src'}", "-Wall", "-Wextra", "-Werror"]


def build_and_run(cxx, out, flags, label):
    build = subprocess.run([cxx, *BASE_FLAGS, *flags, str(SOURCE), "-o", str(out)],
                           capture_output=True, text=True)
    if build.returncode != 0:
        return None, build.stdout + build.stderr
    run = subprocess.run([str(out)], capture_output=True, text=True, timeout=120)
    print(f"--- {label} ---")
    print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=sys.stderr)
    return run.returncode, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--m32", action="store_true", help="fail when the 32-bit build cannot be made")
    options = parser.parse_args()

    failed = False
    with tempfile.TemporaryDirectory(prefix="kisak-bot-motor-") as tmp:
        tmp = Path(tmp)
        code, log = build_and_run(options.cxx, tmp / "sanitized",
                                  ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
                                  "64-bit, sanitized")
        if code is None:
            code, log2 = build_and_run(options.cxx, tmp / "plain", [], "64-bit")
            if code is None:
                print(log + log2, file=sys.stderr)
                print("test_bot_motor: the host build failed", file=sys.stderr)
                return 1
        failed |= code != 0

        code, log = build_and_run(options.cxx, tmp / "i386", ["-m32", "-msse2", "-mfpmath=sse"], "32-bit")
        if code is None:
            if options.m32:
                print(log, file=sys.stderr)
                print("test_bot_motor: the 32-bit build failed (--m32)", file=sys.stderr)
                return 1
            print("test_bot_motor: no 32-bit toolchain, 32-bit run skipped")
        else:
            failed |= code != 0

    if failed:
        print("test_bot_motor: FAILED")
        return 1
    print("test_bot_motor: the bot view motor holds on the host")
    return 0


if __name__ == "__main__":
    sys.exit(main())
