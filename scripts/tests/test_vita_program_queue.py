#!/usr/bin/env python3
"""Check the PS Vita GXM back end's program queue on the host.

``src/gfx_gxm/gxm_program_queue.cpp`` makes the Vita client's shader programs
on a worker thread, so that no frame waits for the memory card or the shader
compiler (docs/vita-client.md, "Programs in the background"). It makes no Vita
call: the read-or-compile is handed in. This test builds it with the host
compiler and runs ``tests/vita_gxm/program_queue_test.cpp`` twice, once with
the address and undefined-behaviour sanitizers and once with the thread
sanitizer: order, a draw's program jumping the queue, dropped requests,
failures, tidying between programs, stopping, and several threads asking at
once.

Usage:
    python3 scripts/tests/test_vita_program_queue.py [--cxx c++] [--timeout 300]
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
    ROOT / "src" / "gfx_gxm" / "gxm_program_queue.cpp",
    ROOT / "tests" / "vita_gxm" / "program_queue_test.cpp",
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

    with tempfile.TemporaryDirectory(prefix="kisak-gxm-program-queue-") as work:
        for name, flags in BUILDS:
            binary = Path(work) / f"program_queue_test_{name}"
            command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread", *flags,
                       "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            if build.returncode:
                sys.stdout.write(build.stdout + build.stderr)
                print(f"FAIL: the program queue test did not build ({name} sanitizer)")
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
                print(f"FAIL: program queue checks failed ({name} sanitizer)")
                return 1
    print("PASS: gxm program queue")
    return 0


if __name__ == "__main__":
    sys.exit(main())
