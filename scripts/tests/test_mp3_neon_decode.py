#!/usr/bin/env python3
"""Compare dr_mp3 built for NEON with dr_mp3 built for scalar VFP.

The PS Vita client builds the MP3 decoder (dr_mp3, in
``src/sound/snd_driver_load_obj.cpp``) with ``-mfpu=neon``; everything else
of the port is scalar VFPv3 (docs/vita.md). This test builds
``tests/sound/mp3_decode_test.cpp`` for 32-bit Arm twice, with the Vita's
float flags and once each ``-mfpu=vfpv3`` and ``-mfpu=neon``, runs both under
an emulator on the same made-up MP3 stream (no game data), and compares the
samples: the NEON build may differ from the scalar one by one step of a
16-bit sample, no more.

It also checks that the comparison means something: every MP3 frame was
decoded, the samples are neither silence nor mostly clipped, the NEON build
holds NEON float instructions and the scalar build holds none.

Without an Arm compiler or an emulator the test is skipped, unless
``--require`` says it must run (CI's armv7 job).

Usage:
    python3 scripts/tests/test_mp3_neon_decode.py [--cxx clang++]
        [--emulator "qemu-arm -L /usr/arm-linux-gnueabihf"] [--require]
"""

import argparse
import array
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "sound" / "mp3_decode_test.cpp"
# The Vita build's float flags (scripts/platform/vita/vita-clang.cmake).
FLAGS = ["--target=arm-linux-gnueabihf", "-mcpu=cortex-a9", "-mthumb", "-mfloat-abi=hard", "-fsigned-char",
         "-ffp-contract=off", "-O2", "-std=c++17"]
MP3_FRAMES = 400
SAMPLES = MP3_FRAMES * 1152 * 2
NEON_FLOAT = re.compile(r"\tv[a-z0-9]+\.f32\t+q")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("clang++") or "clang++")
    parser.add_argument("--emulator", default="qemu-arm -L /usr/arm-linux-gnueabihf")
    parser.add_argument("--objdump", default=shutil.which("arm-linux-gnueabihf-objdump") or "")
    parser.add_argument("--require", action="store_true")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    emulator = shlex.split(args.emulator)
    # Clang builds against the Arm cross GCC's C and C++ libraries.
    missing = [tool for tool in (args.cxx, emulator[0], "arm-linux-gnueabihf-g++") if not shutil.which(tool)]
    if missing:
        print(f"{'FAIL' if args.require else 'SKIP'}: MP3 NEON decode: not found: {', '.join(missing)}")
        return 1 if args.require else 0

    samples = {}
    with tempfile.TemporaryDirectory(prefix="kisak-mp3-neon-") as work:
        for fpu in ("vfpv3", "neon"):
            binary = Path(work) / f"mp3_decode_{fpu}"
            command = [args.cxx, *FLAGS, f"-mfpu={fpu}", "-I", str(ROOT / "deps"), str(SOURCE), "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            if build.returncode:
                sys.stdout.write(build.stdout + build.stderr)
                print(f"FAIL: the MP3 decode test did not build for {fpu}")
                return 1
            if args.objdump:
                listing = subprocess.run([args.objdump, "-d", str(binary)], capture_output=True, text=True,
                                         timeout=args.timeout).stdout
                has_neon = bool(NEON_FLOAT.search(listing))
                if has_neon != (fpu == "neon"):
                    print(f"FAIL: the {fpu} build {'holds' if has_neon else 'holds no'} NEON float instructions")
                    return 1
            output = Path(work) / f"{fpu}.pcm"
            run = subprocess.run([*emulator, str(binary), str(output)], capture_output=True, text=True,
                                 timeout=args.timeout)
            sys.stdout.write(f"{fpu}: {run.stdout}")
            if run.returncode:
                sys.stdout.write(run.stderr)
                print(f"FAIL: the {fpu} build's decode ended with {run.returncode}")
                return 1
            data = array.array("h")
            data.frombytes(output.read_bytes())
            if sys.byteorder != "little":
                data.byteswap()
            samples[fpu] = data

    scalar, neon = samples["vfpv3"], samples["neon"]
    if len(scalar) != SAMPLES or len(neon) != SAMPLES:
        print(f"FAIL: {len(scalar)} and {len(neon)} samples, not {SAMPLES} each")
        return 1
    clipped = sum(1 for value in scalar if abs(value) >= 32767) / SAMPLES
    level = sum(abs(value) for value in scalar) / SAMPLES
    if clipped > 0.3 or level < 1000:
        print(f"FAIL: the stream is no test: {clipped:.1%} of its samples are clipped, mean level {level:.0f}")
        return 1
    worst = 0
    differ = 0
    for a, b in zip(scalar, neon):
        if a != b:
            differ += 1
            worst = max(worst, abs(a - b))
    print(f"{SAMPLES} samples, mean level {level:.0f}, {clipped:.1%} clipped; {differ} differ "
          f"({differ / SAMPLES:.2%}), by {worst} at most")
    if worst > 1 or differ > SAMPLES // 10:
        print("FAIL: the NEON build's samples are not the scalar build's to within one step")
        return 1
    print("PASS: MP3 NEON decode")
    return 0


if __name__ == "__main__":
    sys.exit(main())
