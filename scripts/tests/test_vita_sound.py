#!/usr/bin/env python3
"""Check the PS Vita client's small OpenAL on the host.

``src/vita/client/sound/vita_al.cpp`` answers the OpenAL calls the engine's
sound driver makes, on an integer mixer of its own (docs/vita-client.md,
"Sound"). It uses no Vita call, so this test builds it with the host compiler
and runs ``tests/vita_client/al_test.cpp``, which stands in for the audio
output and reads what the mixer produces: source states, static and queued
buffers, looping, the play offset, resampling, panning, gain changes without a
click, clipping, and the buffer rules the engine depends on.

It builds twice: with AddressSanitizer and UBSan, and with ThreadSanitizer
for the mixer thread against a busy engine thread.

Needs <AL/al.h> (libopenal-dev, or --al-include). No OpenAL library is linked.

Usage:
    python3 scripts/tests/test_vita_sound.py [--cxx c++] [--al-include DIR] [--timeout 300]
"""

import argparse
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "src" / "vita" / "client" / "sound" / "vita_al.cpp",
    ROOT / "tests" / "vita_client" / "al_test.cpp",
]
SANITIZERS = {
    "address": ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
    "thread": ["-fsanitize=thread"],
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--al-include", help="directory holding AL/al.h (default: the system's)")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    include = ["-I", str(ROOT / "src")]
    if args.al_include:
        include += ["-isystem", args.al_include]
    with tempfile.TemporaryDirectory(prefix="kisak-vita-sound-") as work:
        for name, flags in SANITIZERS.items():
            binary = Path(work) / f"al_test_{name}"
            command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                       *flags, *include, *map(str, SOURCES), "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            if build.returncode:
                sys.stdout.write(build.stdout + build.stderr)
                print(f"FAIL: the sound test did not build ({name})")
                return 1
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
            if run.returncode and "unexpected memory mapping" in run.stderr and shutil.which("setarch"):
                # ThreadSanitizer cannot place its shadow under some kernels'
                # address-space randomisation; without it, it can.
                print(f"note: {name} sanitizer could not map its memory; running again without ASLR")
                run = subprocess.run(["setarch", platform.machine(), "-R", str(binary)],
                                     capture_output=True, text=True, timeout=args.timeout)
            sys.stdout.write(run.stdout + run.stderr)
            if run.returncode:
                print(f"FAIL: vita sound checks failed ({name})")
                return 1
    print("PASS: vita sound")
    return 0


if __name__ == "__main__":
    sys.exit(main())
