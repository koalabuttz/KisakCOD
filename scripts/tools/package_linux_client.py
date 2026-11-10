#!/usr/bin/env python3
"""Pack built Linux game binaries into one folder that runs on another PC.

A KisakCOD-mp built on one machine names that machine's SDK folders in its run
path, and its SDL3 and DXVK libraries live wherever they were built. This tool
copies the binaries, the libraries they need from the SDK folders you name,
and a small launcher into one folder:

    <out>/KisakCOD-mp        the binary, as built
    <out>/lib/               its libraries from the SDK folders, by soname
    <out>/run-mp.sh          starts it with lib/ on the library path
    <out>/README.txt         how to run it and what the machine must provide
    <out>/MANIFEST.txt       every file with its size and SHA-256
    <out>/licenses/          the licence texts given with --license

Libraries are found by reading each ELF file's own NEEDED list; nothing is
executed. A needed library that is in none of the --lib-dir folders is left to
the machine that runs the package and is listed in README.txt. The C library,
the compiler's run-time libraries and the graphics driver stack are always left
to that machine, even when a --lib-dir holds them.

The binary's run path must not name folders of the build machine: on another
PC those are folders anyone might create. Build with
-DKISAK_LINUX_PORTABLE_RUNPATH=ON, which makes the run path $ORIGIN/lib.

No game file is read or copied. ELF only: Linux, not macOS.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import stat
import struct
import sys
import tarfile
from pathlib import Path

MANIFEST_HEADER = "KisakCOD Linux package manifest v1"

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_SONAME = 14
DT_RPATH = 15
DT_RUNPATH = 29
PT_LOAD = 1
PT_DYNAMIC = 2

MACHINES = {3: "i386", 40: "arm", 62: "x86_64", 183: "aarch64"}

# Libraries that must come from the machine the package runs on: the C library
# and its parts, the compiler's run-time libraries (bundling an older copy than
# the system's breaks every system library that needs the newer one), and the
# graphics driver stack, which has to match the installed driver.
SYSTEM_ONLY = re.compile(
    r"^(ld-linux[^/]*|ld-musl[^/]*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libnsl|libanl"
    r"|libthread_db|libBrokenLocale|libstdc\+\+|libgcc_s|libatomic"
    r"|libvulkan|libGL|libGLX|libEGL|libGLESv2|libGLdispatch|libOpenGL|libdrm|libgbm)\.so(\.|$)"
)


class PackageError(Exception):
    pass


class Elf:
    """The few ELF facts the packer needs, read without running anything."""

    def __init__(self, path: Path):
        self.path = path
        data = path.read_bytes()
        if len(data) < 52 or data[:4] != b"\x7fELF":
            raise PackageError(f"{path}: not an ELF file")
        if data[4] not in (1, 2):
            raise PackageError(f"{path}: unknown ELF class {data[4]}")
        if data[5] != 1:
            raise PackageError(f"{path}: big-endian ELF files are not supported")
        self.bits = 32 if data[4] == 1 else 64
        self.machine = struct.unpack_from("<H", data, 18)[0]
        self.needed: list[str] = []
        self.soname: str | None = None
        self.runpath: list[str] = []
        self.runpath_tag: str | None = None

        if self.bits == 64:
            phoff = struct.unpack_from("<Q", data, 32)[0]
            phentsize, phnum = struct.unpack_from("<HH", data, 54)
        else:
            phoff = struct.unpack_from("<I", data, 28)[0]
            phentsize, phnum = struct.unpack_from("<HH", data, 42)

        loads: list[tuple[int, int, int]] = []  # vaddr, offset, filesz
        dynamic: tuple[int, int] | None = None
        for index in range(phnum):
            at = phoff + index * phentsize
            if at + phentsize > len(data):
                raise PackageError(f"{path}: program header {index} is past the end of the file")
            if self.bits == 64:
                p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz = struct.unpack_from("<IIQQQQ", data, at)
            else:
                p_type, p_offset, p_vaddr, _paddr, p_filesz = struct.unpack_from("<IIIII", data, at)
            if p_type == PT_LOAD:
                loads.append((p_vaddr, p_offset, p_filesz))
            elif p_type == PT_DYNAMIC:
                dynamic = (p_offset, p_filesz)
        if dynamic is None:
            return  # a static file: needs nothing

        entry = "<QQ" if self.bits == 64 else "<II"
        size = struct.calcsize(entry)
        tags: list[tuple[int, int]] = []
        offset, length = dynamic
        for at in range(offset, min(offset + length, len(data)) - size + 1, size):
            tag, value = struct.unpack_from(entry, data, at)
            if tag == DT_NULL:
                break
            tags.append((tag, value))

        strtab_vaddr = next((value for tag, value in tags if tag == DT_STRTAB), None)
        if strtab_vaddr is None:
            return
        strtab = None
        for vaddr, file_offset, filesz in loads:
            if vaddr <= strtab_vaddr < vaddr + filesz:
                strtab = file_offset + (strtab_vaddr - vaddr)
                break
        if strtab is None:
            raise PackageError(f"{path}: the dynamic string table is in no loaded segment")

        def text(at: int) -> str:
            start = strtab + at
            end = data.find(b"\0", start)
            if start >= len(data) or end < 0:
                raise PackageError(f"{path}: a dynamic string is past the end of the file")
            return data[start:end].decode("utf-8", "surrogateescape")

        for tag, value in tags:
            if tag == DT_NEEDED:
                self.needed.append(text(value))
            elif tag == DT_SONAME:
                self.soname = text(value)
            elif tag in (DT_RUNPATH, DT_RPATH):
                self.runpath_tag = "RUNPATH" if tag == DT_RUNPATH else "RPATH"
                self.runpath.extend(part for part in text(value).split(":") if part)

    @property
    def arch(self) -> str:
        return MACHINES.get(self.machine, f"machine {self.machine}")

    def same_kind(self, other: "Elf") -> bool:
        return self.bits == other.bits and self.machine == other.machine


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def launcher_name(binary_name: str) -> str:
    short = binary_name
    if short.lower().startswith("kisakcod-"):
        short = short[len("kisakcod-"):]
    short = re.sub(r"[^A-Za-z0-9_.-]", "-", short).lower()
    return f"run-{short}.sh"


LAUNCHER = r"""#!/bin/sh
# Starts @BINARY@ from this folder. Written by scripts/tools/package_linux_client.py.
#
#   @LAUNCHER@ --game <game folder> [game options]
#
# The game folder is the one that holds main/ and zone/. It can also be given
# in KISAKCOD_GAME_DIR, or be the folder this is started from. An own
# "+set fs_basepath <folder>" among the options is left alone.
set -u
self=$0
while [ -h "$self" ]; do
    target=$(readlink -- "$self") || exit 1
    case $target in
        /*) self=$target ;;
        *) self=$(dirname -- "$self")/$target ;;
    esac
done
here=$(CDPATH= cd -- "$(dirname -- "$self")" && pwd -P) || exit 1
game=${KISAKCOD_GAME_DIR:-}
if [ "${1:-}" = "--game" ]; then
    if [ "$#" -lt 2 ]; then
        echo "$0: --game needs a folder" >&2
        exit 2
    fi
    game=$2
    shift 2
fi
own_basepath=0
for arg in "$@"; do
    if [ "$arg" = "fs_basepath" ]; then
        own_basepath=1
    fi
done
if [ "$own_basepath" = 0 ]; then
    if [ -z "$game" ] && [ -d ./main ] && [ -d ./zone ]; then
        game=.
    fi
    if [ -z "$game" ]; then
        echo "$0: no game folder. Use: $0 --game <folder with main/ and zone/> [game options]" >&2
        echo "(or set KISAKCOD_GAME_DIR, or start this from the game folder)" >&2
        exit 2
    fi
    if [ ! -d "$game/main" ]; then
        echo "$0: $game has no main folder, so it is not a game folder" >&2
        exit 2
    fi
    game=$(CDPATH= cd -- "$game" && pwd -P) || exit 2
    set -- +set fs_basepath "$game" "$@"
fi
LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
exec "$here/@BINARY@" "$@"
"""


def find_library(soname: str, lib_dirs: list[Path], wanted: Elf, notes: list[str]) -> Path | None:
    for directory in lib_dirs:
        candidate = directory / soname
        if not candidate.is_file():
            continue
        try:
            found = Elf(candidate)
        except PackageError as error:
            notes.append(f"skipped {candidate}: {error}")
            continue
        if not found.same_kind(wanted):
            notes.append(
                f"skipped {candidate}: it is {found.arch} {found.bits}-bit, the binary is "
                f"{wanted.arch} {wanted.bits}-bit")
            continue
        return candidate
    return None


def check_runpath(elf: Elf, allow_build_runpath: bool, notes: list[str]) -> None:
    foreign = [part for part in elf.runpath if not part.startswith("$ORIGIN") and not part.startswith("${ORIGIN}")]
    if not foreign:
        return
    message = (
        f"{elf.path.name}: its {elf.runpath_tag} names folders of the build machine "
        f"({':'.join(foreign)}). On another PC the loader would still look there, in folders "
        "anyone may be able to create. Build with -DKISAK_LINUX_PORTABLE_RUNPATH=ON")
    if not allow_build_runpath:
        raise PackageError(message + ", or pass --allow-build-runpath for a package that stays on this machine.")
    notes.append(message + ". Allowed by --allow-build-runpath: keep this package on the machine that built it.")


def package(args: argparse.Namespace) -> int:
    out = Path(args.out)
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise PackageError(f"{out} exists and is not an empty folder; choose a new folder")

    lib_dirs = [Path(entry) for entry in args.lib_dir]
    for directory in lib_dirs:
        if not directory.is_dir():
            raise PackageError(f"--lib-dir {directory} is not a folder")
    extra_system = set(args.system_only)

    def is_system_only(soname: str) -> bool:
        return soname in extra_system or SYSTEM_ONLY.match(soname) is not None

    binaries: list[Elf] = []
    names: set[str] = set()
    for entry in args.binary:
        path = Path(entry)
        if not path.is_file():
            raise PackageError(f"--binary {path} is not a file")
        elf = Elf(path)
        if path.name in names:
            raise PackageError(f"two binaries are named {path.name}")
        names.add(path.name)
        binaries.append(elf)
    launchers = [launcher_name(elf.path.name) for elf in binaries]
    if len(set(launchers)) != len(launchers):
        raise PackageError(f"two binaries would share a launcher name ({' '.join(launchers)})")
    first = binaries[0]
    for elf in binaries[1:]:
        if not elf.same_kind(first):
            raise PackageError(
                f"{elf.path.name} is {elf.arch} {elf.bits}-bit and {first.path.name} is "
                f"{first.arch} {first.bits}-bit; make one package per architecture")

    notes: list[str] = []
    for elf in binaries:
        check_runpath(elf, args.allow_build_runpath, notes)

    licenses: list[tuple[str, Path]] = []
    for entry in args.license:
        name, separator, source = entry.partition("=")
        if not separator or not name or not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
            raise PackageError(f"--license {entry}: use NAME=FILE, the name of letters, digits, _ . + -")
        if not Path(source).is_file():
            raise PackageError(f"--license {entry}: {source} is not a file")
        licenses.append((name, Path(source)))

    bundled: dict[str, Path] = {}       # soname -> source file
    from_system: dict[str, set[str]] = {}  # soname -> who needs it
    queue: list[tuple[str, str]] = []   # soname, who needs it
    for elf in binaries:
        queue.extend((soname, elf.path.name) for soname in elf.needed)
    for soname in args.also:
        queue.append((soname, "--also"))
    while queue:
        soname, wanted_by = queue.pop(0)
        if "/" in soname:
            raise PackageError(f"{wanted_by} needs {soname}, a path and not a library name")
        if soname in bundled:
            continue
        if is_system_only(soname):
            from_system.setdefault(soname, set()).add(wanted_by)
            continue
        source = find_library(soname, lib_dirs, first, notes)
        if source is None:
            if wanted_by == "--also":
                raise PackageError(f"--also {soname}: not found in any --lib-dir")
            from_system.setdefault(soname, set()).add(wanted_by)
            continue
        bundled[soname] = source
        queue.extend((needed, soname) for needed in Elf(source).needed)

    out.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    def copy(source: Path, target: Path, mode: int) -> None:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(mode)
        written.append(target)

    def write(target: Path, content: str, mode: int) -> None:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        target.chmod(mode)
        written.append(target)

    for elf, launcher in zip(binaries, launchers):
        copy(elf.path, out / elf.path.name, 0o755)
        write(out / launcher,
              LAUNCHER.replace("@BINARY@", elf.path.name).replace("@LAUNCHER@", launcher), 0o755)
    for soname in sorted(bundled):
        copy(bundled[soname], out / "lib" / soname, 0o644)
    for name, source in licenses:
        copy(source, out / "licenses" / f"{name}.txt", 0o644)

    label = args.label or "unlabelled"
    readme = [
        "KisakCOD for Linux",
        "==================",
        "",
        f"Build: {label}",
        f"Made for: {first.arch} ({first.bits}-bit) Linux",
        "",
        "This folder holds the game program and the libraries it was built with.",
        "It holds no game files: you need your own Call of Duty 4 installation.",
        "",
        "To run",
        "------",
        "",
    ]
    for elf, launcher in zip(binaries, launchers):
        readme.append(f"  ./{launcher} --game \"<your Call of Duty 4 folder>\"      starts {elf.path.name}")
    readme += [
        "",
        "The game folder is the one that holds main/ and zone/. Game options go after it,",
        "for example: +set r_fullscreen 0. The game clients keep settings and player",
        "profiles in ~/.local/share/KisakCOD (or $XDG_DATA_HOME/KisakCOD), not in the",
        "game folder.",
        "",
        "What this PC must provide",
        "-------------------------",
        "",
        "These libraries are not in this folder and are taken from the system:",
        "",
    ]
    for soname in sorted(from_system):
        readme.append(f"  {soname}    (needed by {', '.join(sorted(from_system[soname]))})")
    if not from_system:
        readme.append("  (none)")
    readme += [
        "",
        "The multiplayer and singleplayer clients also need a graphics driver with Vulkan.",
        "",
        "In this folder",
        "--------------",
        "",
    ]
    for soname in sorted(bundled):
        readme.append(f"  lib/{soname}")
    if not bundled:
        readme.append("  (no libraries)")
    if licenses:
        readme += ["", "Licence texts of the libraries are in licenses/."]
    if notes:
        readme += ["", "Notes from packing", "------------------", ""]
        readme += [f"  {note}" for note in notes]
    readme.append("")
    write(out / "README.txt", "\n".join(readme), 0o644)

    manifest = [MANIFEST_HEADER, f"label: {label}", f"arch: {first.arch} {first.bits}-bit"]
    for path in sorted(written):
        manifest.append(f"{sha256(path)}  {path.stat().st_size}  {path.relative_to(out).as_posix()}")
    manifest.append("")
    (out / "MANIFEST.txt").write_text("\n".join(manifest), encoding="utf-8")
    (out / "MANIFEST.txt").chmod(0o644)

    print(f"packed {len(binaries)} binary(ies) and {len(bundled)} library(ies) into {out}")
    for soname in sorted(bundled):
        print(f"  lib/{soname}  <-  {bundled[soname]}")
    if from_system:
        print("left to the system: " + " ".join(sorted(from_system)))
    for note in notes:
        print(f"note: {note}")

    if args.archive:
        archive = out.parent / f"{out.name}.tar.gz"
        if archive.exists():
            raise PackageError(f"{archive} exists; remove it or choose another --out")
        with tarfile.open(archive, "w:gz") as tar:
            for path in sorted(out.rglob("*")):
                if not path.is_file():
                    continue
                info = tar.gettarinfo(str(path), arcname=f"{out.name}/{path.relative_to(out).as_posix()}")
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mode = stat.S_IMODE(path.stat().st_mode)
                with path.open("rb") as handle:
                    tar.addfile(info, handle)
        print(f"archive: {archive}")
    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pack built Linux game binaries, their SDK libraries and a launcher into one folder.")
    parser.add_argument("--binary", action="append", default=[], required=True,
                        help="a built binary, for example KisakCOD-mp (repeat for more of the same architecture)")
    parser.add_argument("--lib-dir", action="append", default=[],
                        help="a folder to take needed libraries from, for example the SDL3 SDK's lib "
                             "folder or DXVK's (repeat; earlier folders win)")
    parser.add_argument("--out", required=True, help="the folder to make; it must not exist yet, or be empty")
    parser.add_argument("--also", action="append", default=[],
                        help="a library, by soname, to pack although no file lists it as needed "
                             "(one loaded by name at run time)")
    parser.add_argument("--system-only", action="append", default=[],
                        help="a soname to leave to the system even when a --lib-dir holds it")
    parser.add_argument("--license", action="append", default=[],
                        help="NAME=FILE: copy a library's licence text to licenses/NAME.txt")
    parser.add_argument("--label", default="",
                        help="what build this is, for README.txt and MANIFEST.txt (for example a git hash)")
    parser.add_argument("--allow-build-runpath", action="store_true",
                        help="pack a binary whose run path names folders of the build machine; "
                             "only for a package that stays on that machine")
    parser.add_argument("--archive", action="store_true", help="also write <out>.tar.gz next to the folder")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return package(args)
    except PackageError as error:
        print(f"package_linux_client: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
