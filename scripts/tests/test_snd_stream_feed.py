#!/usr/bin/env python3
"""Check the OpenAL driver's stream feed on the host.

``src/sound/snd_stream_feed.h`` carries a streamed sound's decoded frames
from its decoder to its channel, and holds the thread that decodes ahead with
``snd_streamThread`` (docs/vita-client.md, "Streamed sounds"). It uses no
engine type and no OpenAL. This test builds
``tests/sound/stream_feed_test.cpp`` with the host compiler and runs it
twice, once with the address and undefined-behaviour sanitizers and once
with the thread sanitizer: a stream from its first frame to its last, a
full feed, loops, a start part of the way in by a seek and by reading up to
it, streams with nothing in them,
the thread decoding for several feeds, streams closed while the thread
decodes them, a close that does not wait for a whole chunk, and the thread
and the channel's thread decoding in turns.

``scripts/tests/test_openal_channel_stop.py`` runs the driver's own side of
it against OpenAL.

Usage:
    python3 scripts/tests/test_snd_stream_feed.py [--cxx c++] [--timeout 300]
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
    ROOT / "tests" / "sound" / "stream_feed_test.cpp",
]
BUILDS = [
    ("address", ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]),
    ("thread", ["-fsanitize=thread"]),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-snd-stream-feed-") as work:
        for name, flags in BUILDS:
            binary = Path(work) / f"stream_feed_test_{name}"
            command = [args.cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread", *flags,
                       "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            if build.returncode:
                sys.stdout.write(build.stdout + build.stderr)
                print(f"FAIL: the stream feed test did not build ({name} sanitizer)")
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
                print(f"FAIL: stream feed checks failed ({name} sanitizer)")
                return 1
    print("PASS: sound stream feed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
