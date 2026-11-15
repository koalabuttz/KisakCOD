#!/usr/bin/env python3
"""Check the writer of the Vita's console.log on the host.

``src/vita/vita_log_writer.h`` takes the lines the game prints into memory
and writes them to the memory card from a thread of its own, so a slow write
does not hold the thread that printed. It makes no engine call and no Vita
call, so this builds ``tests/vita_client/log_writer_test.cpp`` with the host compiler
(ASan and UBSan on, and once more with TSan) and runs it against a file whose
writes can be made to hang.

Usage:
    python3 scripts/tests/test_vita_log_writer.py [--cxx c++] [--timeout 300]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_client" / "log_writer_test.cpp",
]


def build_and_run(cxx: str, sanitizers: str, work: str, timeout: int) -> tuple[bool, str]:
    binary = Path(work) / ("log_writer_test_" + sanitizers.replace(",", "_"))
    command = [cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
               f"-fsanitize={sanitizers}", "-fno-sanitize-recover=all",
               "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
    build = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if build.returncode:
        return False, build.stdout + build.stderr + "the log writer test did not build\n"
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout)
    return run.returncode == 0, run.stdout + run.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-log-writer-") as work:
        ok, output = build_and_run(args.cxx, "address,undefined", work, args.timeout)
        sys.stdout.write(output)
        if not ok:
            print("FAIL: vita log writer checks failed")
            return 1
        # The same checks watched for two threads at one piece of memory.
        # Some hosts cannot run ThreadSanitizer at all (it stops before main
        # over the machine's memory layout); that is said and not a failure.
        ok, output = build_and_run(args.cxx, "thread", work, args.timeout)
        if not ok and ("unexpected memory mapping" in output or "did not build" in output):
            print("note: ThreadSanitizer does not run on this host; its pass was left out")
        else:
            sys.stdout.write(output)
            if not ok:
                print("FAIL: vita log writer checks failed under ThreadSanitizer")
                return 1
    print("PASS: vita log writer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
