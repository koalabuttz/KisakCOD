#!/usr/bin/env python3
"""Check the PS Vita's pack of compiled programs on the host.

``scripts/platform/vita/vita_pack_programs.py`` puts a program folder's
``.gxp`` files into one ``programs.pack``; the Vita client reads it with
``src/gfx_gxm/gxm_program_pack.h`` (docs/vita.md, "A pack of programs").
This makes a folder of stand-in programs (random bytes: no game data), packs
it, and has ``tests/vita_gxm/program_pack_test.cpp``, built with the host
compiler (ASan and UBSan on), read every program back through the header.
Files that are not packs, and packs whose index is wrong, must be refused.

Usage:
    python3 scripts/tests/test_vita_program_pack.py [--cxx c++] [--timeout 300]
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "vita_gxm" / "program_pack_test.cpp"
PACKER = ROOT / "scripts" / "platform" / "vita" / "vita_pack_programs.py"

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {what}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-vita-program-pack-") as temporary:
        work = Path(temporary)
        binary = work / "program_pack_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), str(SOURCE), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the program pack test did not build")
            return 1

        def tool(*arguments):
            run = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, timeout=args.timeout)
            return run.returncode, run.stdout

        def packer(*arguments):
            run = subprocess.run([sys.executable, str(PACKER), *map(str, arguments)], capture_output=True, text=True,
                                 timeout=args.timeout)
            return run.returncode, run.stdout + run.stderr

        # A folder as a run leaves it: programs, markers, sources, a part.
        folder = work / "gxp"
        folder.mkdir()
        programs = {}
        for key in [0, 1, 0x00000000ffffffff, 0x0000000100000000, 0x7fffffffffffffff, 0x8000000000000000,
                    0xffffffffffffffff] + [int.from_bytes(os.urandom(8), "little") for _ in range(200)]:
            programs[key] = os.urandom(1 + int.from_bytes(os.urandom(2), "little") % 3000)
            (folder / f"{key:016x}.gxp").write_bytes(programs[key])
        (folder / "no-vsync").write_bytes(b"")
        (folder / "0123456789abcdef.cg").write_text("float4 main() : COLOR { return 0; }")
        (folder / "0123456789abcdef.gxp.tmp").write_bytes(b"half a program")
        (folder / "00000000000000aa.gxp").write_bytes(b"")
        (folder / "ABCDEF0123456789.gxp").write_bytes(b"upper case is not a name the client writes")

        pack = work / "programs.pack"
        code, output = packer("pack", "--out", pack, folder)
        check(code == 0 and f"{len(programs)} programs" in output, f"packing: {output}")
        code, output = packer("list", pack)
        check(code == 0 and output.startswith(f"{len(programs)} programs, {sum(map(len, programs.values()))} bytes"),
              f"listing: {output}")
        code, listed = tool("list", pack)
        expected = "".join(f"{key:016x} {len(programs[key])}\n" for key in sorted(programs))
        check(code == 0 and listed.decode() == expected, "the header lists every program in rising order of key")
        for key, program in programs.items():
            code, got = tool("get", pack, f"{key:016x}")
            if code != 0 or got != program:
                check(False, f"program {key:016x} read back")
                break
        check(tool("get", pack, "0000000000000002")[0] == 4, "a key the pack does not hold")
        check(tool("list", work / "absent.pack")[0] == 2, "no file")

        # A later source wins, and a pack is a source too.
        second = work / "second"
        second.mkdir()
        (second / f"{1:016x}.gxp").write_bytes(b"the newer program one")
        (second / f"{2:016x}.gxp").write_bytes(b"a program the first folder lacks")
        merged = work / "merged.pack"
        code, output = packer("pack", "--out", merged, pack, second)
        check(code == 0 and f"{len(programs) + 1} programs" in output, f"merging: {output}")
        check(tool("get", merged, f"{1:016x}")[1] == b"the newer program one", "the later source wins")
        check(tool("get", merged, f"{2:016x}")[1] == b"a program the first folder lacks", "the added program")
        check(tool("get", merged, f"{0:016x}")[1] == programs[0], "the first source's program stays")

        # An empty pack is a pack; everything else here is refused.
        def refused(name, data):
            path = work / name
            path.write_bytes(data)
            check(tool("list", path)[0] == 3, f"{name} should be refused")

        (work / "empty.pack").write_bytes(b"KGXPACK1" + struct.pack("<II", 0, 0))
        check(tool("list", work / "empty.pack") == (0, b""), "an empty pack")
        good = pack.read_bytes()
        refused("short.pack", good[:10])
        refused("magic.pack", b"KGXPACK2" + good[8:])
        refused("reserved.pack", good[:12] + struct.pack("<I", 1) + good[16:])
        refused("count.pack", good[:8] + struct.pack("<I", 40000) + good[12:])
        refused("cut-index.pack", good[:16 + 16 * 3 + 5])
        refused("cut-programs.pack", good[:-1])
        entry = lambda key, offset, size: struct.pack("<QII", key, offset, size)
        head = lambda count: b"KGXPACK1" + struct.pack("<II", count, 0)
        refused("order.pack", head(2) + entry(5, 48, 1) + entry(4, 49, 1) + b"ab")
        refused("twice.pack", head(2) + entry(5, 48, 1) + entry(5, 49, 1) + b"ab")
        refused("zero.pack", head(1) + entry(5, 32, 0) + b"a")
        refused("in-index.pack", head(1) + entry(5, 16, 4) + b"abcd")
        refused("beyond.pack", head(1) + entry(5, 32, 5) + b"abcd")
        refused("wrap.pack", head(1) + entry(5, 0xfffffff0, 0x20) + b"abcd")
        refused("huge.pack", head(1) + entry(5, 32, (4 << 20) + 1) + b"abcd")

        code, output = packer("pack", "--out", work / "none.pack", work / "sources-that-are-not-there")
        check(code != 0, "a source that is not there")
        code, output = packer("list", work / "magic.pack")
        check(code != 0 and "not a pack" in output, "the script refuses what is not a pack")
    if failures:
        print("FAIL: vita program pack")
        return 1
    print("PASS: vita program pack")
    return 0


if __name__ == "__main__":
    sys.exit(main())
