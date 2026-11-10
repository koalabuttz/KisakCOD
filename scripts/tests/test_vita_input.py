#!/usr/bin/env python3
"""Check the PS Vita client's control mapping and text entry on the host.

``src/vita/client/input/vita_input_map.cpp`` turns the Vita's buttons, sticks
and touch panels into engine keys and mouse motion (docs/vita-client.md,
"Controls"). It makes no engine call and no Vita call, so this test builds it
with the host compiler and runs ``tests/vita_client/input_map_test.cpp``, which
feeds it made-up readings and reads the keys and the motion that come out: the
default layout, the move stick's four keys, the look stick's speed and curve,
the menu's cursor, clicks, arrows and wheel, the touch areas, what is let go
when a menu opens or closes, and the controls menu's key capture.

``src/vita/client/input/vita_text_entry.cpp`` decides when the on-screen
keyboard opens and what the game is handed when it closes. It is built the same
way and run by ``tests/vita_client/text_entry_test.cpp``, which plays the game
and the keyboard: a menu field, the chat line and a console command, a close
without Enter, a field that goes away while the keyboard is up, a text longer
than a field, a keyboard that does not open, and the keyboard's UTF-16 to and
from the engine's one-byte characters.

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
INPUT = ROOT / "src" / "vita" / "client" / "input"
TESTS = ROOT / "tests" / "vita_client"
# Each check: its name, the unit under test and the test that drives it.
CHECKS = [
    ("input", INPUT / "vita_input_map.cpp", TESTS / "input_map_test.cpp"),
    ("text entry", INPUT / "vita_text_entry.cpp", TESTS / "text_entry_test.cpp"),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-input-") as work:
        for index, (name, unit, test) in enumerate(CHECKS):
            binary = Path(work) / f"check{index}"
            command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-I", str(ROOT / "src"), str(unit), str(test), "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            if build.returncode:
                sys.stdout.write(build.stdout + build.stderr)
                print(f"FAIL: the vita {name} test did not build")
                return 1
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
            sys.stdout.write(run.stdout + run.stderr)
            if run.returncode:
                print(f"FAIL: vita {name} checks failed")
                return 1
    print("PASS: vita input")
    return 0


if __name__ == "__main__":
    sys.exit(main())
