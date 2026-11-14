#!/usr/bin/env python3
"""Pack a PS Vita program folder's compiled programs into one file.

The Vita client keeps each compiled GXM program in a file of its own,
``<16 hex digits>.gxp`` in ``ux0:data/KisakCOD/gxp``. With ``programs.pack``
in that folder it looks there first (src/gfx_gxm/gxm_program_pack.h): one
open in place of hundreds, and a pack made from the folder of a Vita3K run
of the same build saves a console the minutes of compiling them, since
Vita3K runs the Vita's own compiler (docs/vita.md, "A pack of programs").

    vita_pack_programs.py pack --out programs.pack FOLDER_OR_PACK [...]
        Every <16 hex digits>.gxp of each folder, and every program of each
        pack; where two sources hold the same program the later one wins.
    vita_pack_programs.py list programs.pack

The programs come from the game's shaders: a pack stays on the owner's
machines like the game's files.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

MAGIC = b"KGXPACK1"
NAME = re.compile(r"^([0-9a-f]{16})\.gxp$")
MAX_PROGRAMS = 32768
MAX_PROGRAM_BYTES = 4 << 20


def read_pack(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 16 or data[:8] != MAGIC:
        raise SystemExit(f"{path}: not a pack")
    count, zero = struct.unpack_from("<II", data, 8)
    if zero or count > MAX_PROGRAMS or 16 + count * 16 > len(data):
        raise SystemExit(f"{path}: not a pack")
    programs = {}
    for i in range(count):
        key, offset, size = struct.unpack_from("<QII", data, 16 + i * 16)
        if offset + size > len(data) or not size:
            raise SystemExit(f"{path}: entry {i} is outside the file")
        programs[key] = data[offset:offset + size]
    return programs


def write_pack(path: Path, programs: dict) -> None:
    if len(programs) > MAX_PROGRAMS:
        raise SystemExit(f"{len(programs)} programs: a pack holds up to {MAX_PROGRAMS}")
    keys = sorted(programs)
    offset = 16 + len(keys) * 16
    index = bytearray()
    for key in keys:
        index += struct.pack("<QII", key, offset, len(programs[key]))
        offset += len(programs[key])
    with open(path, "wb") as file:
        file.write(MAGIC + struct.pack("<II", len(keys), 0) + index)
        for key in keys:
            file.write(programs[key])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("pack")
    pack.add_argument("--out", required=True)
    pack.add_argument("sources", nargs="+")
    listing = commands.add_parser("list")
    listing.add_argument("pack")
    args = parser.parse_args()

    if args.command == "list":
        programs = read_pack(Path(args.pack))
        print(f"{len(programs)} programs, {sum(map(len, programs.values()))} bytes of programs")
        return 0

    programs = {}
    for source in map(Path, args.sources):
        if source.is_dir():
            found = 0
            for path in sorted(source.iterdir()):
                match = NAME.match(path.name)
                if not match or not path.is_file():
                    continue
                data = path.read_bytes()
                if not data or len(data) > MAX_PROGRAM_BYTES:
                    print(f"left out: {path.name} ({len(data)} bytes)")
                    continue
                programs[int(match[1], 16)] = data
                found += 1
            print(f"{source}: {found} programs")
        elif source.is_file():
            packed = read_pack(source)
            programs.update(packed)
            print(f"{source}: {len(packed)} programs")
        else:
            raise SystemExit(f"{source}: neither a folder nor a pack")
    if not programs:
        raise SystemExit("no program found")
    write_pack(Path(args.out), programs)
    print(f"{args.out}: {len(programs)} programs, {Path(args.out).stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
