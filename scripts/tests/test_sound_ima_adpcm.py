#!/usr/bin/env python3
"""Check the IMA ADPCM blocks loaded sounds can be kept in.

``src/sound/snd_ima_adpcm.h`` decodes and encodes IMA ADPCM in the Microsoft
block layout and plans how a loaded sound is kept packed (docs/vita-client.md,
"Sound"). The OpenAL sound driver and the PS Vita client's mixer both build
it. It makes no engine call, so this test builds
``tests/sound/ima_adpcm_test.cpp`` with the host compiler, AddressSanitizer and
UBSan, and runs it: the decoder against the vendored dr_wav on random blocks,
the encoder's signal to noise, and every kind of sound through the packer with
buffers of the exact size.

No game data is read.

Usage:
    python3 scripts/tests/test_sound_ima_adpcm.py [--cxx c++] [--timeout 300]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "sound" / "ima_adpcm_test.cpp"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-ima-adpcm-") as work:
        binary = Path(work) / "ima_adpcm_test"
        # dr_wav is a system header here: its own warnings are not this test's.
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), "-isystem", str(ROOT / "deps"), str(SOURCE), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the IMA ADPCM test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: IMA ADPCM checks failed")
            return 1
    print("PASS: IMA ADPCM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
