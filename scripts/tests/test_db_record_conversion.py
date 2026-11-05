#!/usr/bin/env python3
"""Check the 64-bit fastfile record conversion without game data.

Compiles the production src/database/db_stream.cpp and db_stream_load.cpp,
unmodified, for x86_64 together with tests/linux_db_convert/db_convert_test.cpp,
which feeds them retail (32-bit) record bytes from memory and checks:

  * pointer fields are widened, with the -1/-2 "data follows" markers kept;
  * records expand to the native layout and the stream advances natively;
  * retail block offsets map to the native record, field or one-past-the-end
    address, including after retail and native alignment diverge;
  * record heads (the xanim loaders' partial records), inserted pointer slots
    and aliases, the temp block rewind, runtime and delayed blocks;
  * a block that is too small stops the load with an error.

The build uses AddressSanitizer and UndefinedBehaviorSanitizer. The i386 build
needs no run: there the generated layout tables are checked at compile time.

--arch aarch64 builds the same test for 64-bit Arm (docs/linux-arm64.md), with
UndefinedBehaviorSanitizer in trap mode only (no sanitizer runtime to cross-link,
and AddressSanitizer does not work under qemu-user). On an x86 host it
cross-compiles and runs the result through --emulator (default
"qemu-aarch64 -L /usr/aarch64-linux-gnu").

Usage:
    python3 scripts/tests/test_db_record_conversion.py [--cxx clang++] [--arch x86_64|aarch64] [--emulator CMD]
"""

import argparse
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    ROOT / "tests" / "linux_db_convert" / "db_convert_test.cpp",
    ROOT / "src" / "database" / "db_stream.cpp",
    ROOT / "src" / "database" / "db_stream_load.cpp",
)
COMMON_FLAGS = (
    "-std=gnu++20", "-g", "-O1", "-fms-extensions", "-fno-strict-aliasing",
    "-DDEDICATED", "-DKISAK_DEDICATED", "-DKISAK_MP", "-D_DEBUG", "-D_GNU_SOURCE",
    "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer",
    "-Wno-everything",
)
ARCH_FLAGS = {
    "x86_64": ("-m64", "-msse2", "-fsanitize=address,undefined"),
    # Matches KISAK_LINUX_ARCH=aarch64: plain char stays signed and no fused
    # multiply-add, as on x86.
    "aarch64": ("--target=aarch64-linux-gnu", "-fsigned-char", "-ffp-contract=off", "-fsanitize=undefined", "-fsanitize-trap=undefined"),
}
DEFAULT_AARCH64_EMULATOR = "qemu-aarch64 -L /usr/aarch64-linux-gnu"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--arch", choices=sorted(ARCH_FLAGS), default="x86_64")
    parser.add_argument("--emulator", default=None,
                        help="command prefix that runs the test binary (default: none, or qemu-aarch64 "
                             "for --arch aarch64 on a non-Arm host)")
    args = parser.parse_args()
    emulator = args.emulator
    if emulator is None:
        native_arm = platform.machine().lower() in ("aarch64", "arm64")
        emulator = DEFAULT_AARCH64_EMULATOR if args.arch == "aarch64" and not native_arm else ""

    with tempfile.TemporaryDirectory(prefix="kisak-db-convert-") as temporary:
        binary = Path(temporary) / "db_convert_test"
        command = [args.cxx, *ARCH_FLAGS[args.arch], *COMMON_FLAGS, "-I", str(ROOT / "src"), "-I", str(ROOT / "deps"),
                   *map(str, SOURCES), "-o", str(binary)]
        print("+ " + " ".join(command), flush=True)
        built = subprocess.run(command, check=False)
        if built.returncode != 0:
            print("FAIL: the conversion test did not build", flush=True)
            return 1
        try:
            ran = subprocess.run([*shlex.split(emulator), str(binary)], check=False, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print(f"FAIL: the conversion test ran longer than {args.timeout}s", flush=True)
            return 1
        if ran.returncode != 0:
            print(f"FAIL: the conversion test exited {ran.returncode}", flush=True)
            return 1
    print(f"PASS: 64-bit fastfile record conversion ({args.arch})", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
