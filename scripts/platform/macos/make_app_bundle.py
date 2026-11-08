#!/usr/bin/env python3
"""Packs a macOS KisakCOD-mp build into an app bundle (docs/macos.md).

KisakCOD-mp.app/
  Contents/Info.plist
  Contents/MacOS/KisakCOD-mp          the executable, with one run path:
                                      @executable_path/../Frameworks
  Contents/Frameworks/*.dylib         the libraries it loads through @rpath

The build's own run paths (absolute build-tree folders) are taken out, so the
bundle loads only what it carries and the system's libraries. Every library
the executable needs through @rpath must be passed with --library; the script
stops when one is missing or left over. The bundle is signed ad hoc (no
identity), which is what Apple silicon needs to run it; a signed and notarised
release is a later step.

Runs on macOS only (otool, install_name_tool, codesign). No game data.

Usage:
  python3 scripts/platform/macos/make_app_bundle.py \\
      --executable out/KisakCOD-mp --output out/bundle \\
      --library /path/to/libSDL3.0.dylib --library /path/to/libd3d9.dylib ...
"""
import argparse
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

FRAMEWORKS_RPATH = "@executable_path/../Frameworks"
# Libraries every Mac has; the bundle does not carry them.
SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")


def tool(*command):
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        sys.exit(f"make_app_bundle: {' '.join(command)} failed:\n{completed.stdout}{completed.stderr}")
    return completed.stdout


def install_id(library):
    """The library's own install name (otool -D), or '' for a file without one."""
    lines = tool("otool", "-D", str(library)).splitlines()
    return lines[1].strip() if len(lines) > 1 else ""


def needed(binary):
    """Install names the binary links (otool -L), without its own id."""
    lines = tool("otool", "-L", str(binary)).splitlines()[1:]
    names = [line.strip().split(" (compatibility", 1)[0] for line in lines if line.strip()]
    own = install_id(binary) if binary.suffix == ".dylib" else ""
    return [name for name in names if name != own]


def rpaths(binary):
    """The binary's LC_RPATH entries, in order (otool -l)."""
    found, lines = [], tool("otool", "-l", str(binary)).splitlines()
    for index, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for following in lines[index + 1:index + 4]:
                parts = following.split()
                if parts and parts[0] == "path":
                    found.append(parts[1])
                    break
    return found


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--executable", type=Path, required=True, help="the built KisakCOD-mp")
    parser.add_argument("--output", type=Path, required=True, help="folder to write KisakCOD-mp.app into")
    parser.add_argument("--library", type=Path, action="append", default=[],
                        help="a library the executable loads through @rpath (repeat for each)")
    parser.add_argument("--bundle-id", default="io.github.kisakcod.mp", help="CFBundleIdentifier")
    parser.add_argument("--version", default="0.1", help="CFBundleShortVersionString")
    parser.add_argument("--minimum-macos", default="13.3", help="LSMinimumSystemVersion (the build's deployment target)")
    options = parser.parse_args(argv)

    executable = options.executable.resolve()
    name = executable.name
    app = options.output / f"{name}.app"
    if app.exists():
        shutil.rmtree(app)
    macos, frameworks = app / "Contents" / "MacOS", app / "Contents" / "Frameworks"
    macos.mkdir(parents=True)
    frameworks.mkdir()

    bundled_exe = macos / name
    shutil.copy2(executable, bundled_exe)

    # Each library goes in under the leaf of its @rpath install name, so the
    # executable's load commands find it unchanged.
    carried = {}
    for library in options.library:
        real = library.resolve()
        identity = install_id(real)
        if not identity.startswith("@rpath/"):
            sys.exit(f"make_app_bundle: {library} has install name '{identity}', not @rpath/...")
        leaf = identity[len("@rpath/"):]
        if leaf in carried:
            sys.exit(f"make_app_bundle: two libraries are named {leaf}")
        target = frameworks / leaf
        shutil.copy2(real, target)
        target.chmod(0o755)
        carried[leaf] = target

    # Everything loaded through @rpath, by the executable or by a carried
    # library, must be carried; nothing carried may go unused.
    wanted = set()
    for binary in [bundled_exe, *carried.values()]:
        for dependency in needed(binary):
            if dependency.startswith("@rpath/"):
                wanted.add(dependency[len("@rpath/"):])
            elif not dependency.startswith(SYSTEM_PREFIXES):
                sys.exit(f"make_app_bundle: {binary.name} needs {dependency}, outside the bundle and the system")
    if wanted != set(carried):
        missing, extra = sorted(wanted - set(carried)), sorted(set(carried) - wanted)
        sys.exit(f"make_app_bundle: libraries missing {missing}, not needed {extra}")

    # Only the bundle's own folder: no build-tree paths survive.
    for binary in [bundled_exe, *carried.values()]:
        for path in rpaths(binary):
            tool("install_name_tool", "-delete_rpath", path, str(binary))
    tool("install_name_tool", "-add_rpath", FRAMEWORKS_RPATH, str(bundled_exe))

    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": name,
        "CFBundleExecutable": name,
        "CFBundleIdentifier": options.bundle_id,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": options.version,
        "CFBundleVersion": options.version,
        "LSApplicationCategoryType": "public.app-category.action-games",
        "LSMinimumSystemVersion": options.minimum_macos,
        "NSHighResolutionCapable": True,
    }
    with open(app / "Contents" / "Info.plist", "wb") as handle:
        plistlib.dump(info, handle)

    # install_name_tool leaves the old signatures invalid: sign again, the
    # libraries first, then the bundle (which seals them).
    for library in carried.values():
        tool("codesign", "--force", "--sign", "-", str(library))
    tool("codesign", "--force", "--sign", "-", str(app))
    tool("codesign", "--verify", "--strict", "--deep", str(app))

    print(f"{app}")
    print(f"  executable: Contents/MacOS/{name}, run path {FRAMEWORKS_RPATH}")
    for leaf in sorted(carried):
        print(f"  library:    Contents/Frameworks/{leaf}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
