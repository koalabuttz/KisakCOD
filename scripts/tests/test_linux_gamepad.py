#!/usr/bin/env python3
"""Check the desktop client's controller mapping on the host.

``src/linux/client/input/gamepad_map.cpp`` puts a full-size game controller on
the PS Vita client's control mapping (``src/vita/client/input/
vita_input_map.cpp``) and adds the bumpers, the stick clicks, the triggers and
the cursor it shares with the real mouse (docs/linux-mp-client.md, "Game
controllers"). Neither file makes an engine call or an SDL call, so this test
builds both with the host compiler and runs
``tests/linux_mp_client/gamepad_map_test.cpp``, which feeds made-up readings
and reads the keys and the motion that come out.

Built with AddressSanitizer and UBSan. No controller, no SDL and no game file
is involved; reading a real controller through SDL is checked by the client
window test in the hosted x86_64 client job.

Usage:
    python3 scripts/tests/test_linux_gamepad.py [--cxx c++] [--timeout 300]
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
    ROOT / "src" / "linux" / "client" / "input" / "gamepad_map.cpp",
    ROOT / "tests" / "linux_mp_client" / "gamepad_map_test.cpp",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-gamepad-") as work:
        binary = Path(work) / "gamepad_map_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the gamepad mapping test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: gamepad mapping checks failed")
            return 1
    print("PASS: gamepad mapping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
