#!/usr/bin/env python3
"""Build and run tests/linux_phys_objid/phys_objid_test.cpp for i386 and x86_64.

Checks physics object ids (src/physics/phys_objid.cpp): game structs keep a
dxBody in a 32-bit field. On i386 the id is the pointer, as in the original
game; on x86_64 it is a slot in a table of live bodies. Asset-free.

Usage: python3 scripts/tests/test_linux_phys_objid.py [--cxx clang++]
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "tests" / "linux_phys_objid" / "phys_objid_test.cpp"
SOURCE = ROOT / "src" / "physics" / "phys_objid.cpp"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    ok = True
    with tempfile.TemporaryDirectory(prefix="kisak-phys-objid-") as workdir:
        for arch in ("-m32", "-m64"):
            binary = Path(workdir) / f"phys_objid_test{arch}"
            # _DEBUG turns iassert on, as in the Debug engine builds.
            build = [options.cxx, arch, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_DEBUG",
                     "-I", str(ROOT / "src"), str(TEST), str(SOURCE), "-o", str(binary)]
            print("+", " ".join(build), flush=True)
            if subprocess.run(build, timeout=options.timeout, check=False).returncode != 0:
                print(f"FAIL: phys_objid_test{arch} did not build")
                ok = False
                continue
            completed = subprocess.run([str(binary)], timeout=options.timeout, capture_output=True,
                                       text=True, check=False)
            text = completed.stdout + completed.stderr
            print(f"--- {arch}")
            print(text, end="")
            if completed.returncode != 0 or "FAIL:" in text or "phys_objid_test: done" not in text:
                print(f"FAIL: phys_objid_test{arch} exited {completed.returncode}")
                ok = False
    print("PASS: physics object ids" if ok else "FAIL: physics object ids")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
