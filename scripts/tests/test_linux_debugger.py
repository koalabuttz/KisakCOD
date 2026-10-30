#!/usr/bin/env python3
"""Check the native dedicated debugger boundary, without game assets or a UI.

Links the real Linux implementation with checked engine-service doubles. This
verifies initialization, opcode restoration and cleanup, not a complete VM run.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kisak-linux-debugger-") as temp:
        for variant, flags in (("debug", ["-O0", "-D_DEBUG"]),
                               ("release", ["-O2", "-DNDEBUG"])):
            executable = Path(temp) / variant
            command = [args.cxx, "-m32", "-std=c++20", "-fms-extensions",
                       "-msse2", "-ffunction-sections", "-fdata-sections",
                       "-DKISAK_MP", "-DKISAK_DEDICATED", "-DDEDICATED",
                       "-Wno-missing-declarations", "-Wno-ignored-attributes",
                       "-I", str(ROOT / "src"), "-I", str(ROOT / "deps"),
                       *flags, str(ROOT / "src/linux/script_debugger.cpp"),
                       str(ROOT / "scripts/tests/linux_debugger_test.cpp"),
                       "-Wl,--gc-sections", "-pthread", "-o", str(executable)]
            subprocess.run(command, check=True, timeout=60)
            print(f"Native debugger checks: {variant}", flush=True)
            subprocess.run([str(executable)], check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
