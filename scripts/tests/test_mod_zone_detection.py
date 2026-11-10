#!/usr/bin/env python3
"""Check that a server marks a zone as a mod zone only when it opened it from the mod folder.

DB_TryLoadXFileInternal first tries the mod folder (when fs_game is set) and
falls back to the game's own zone folder. The result decides whether
DB_ReferencedFFNameList advertises the zone as "<fs_game>/<zone>". A client
asks the server for every advertised mod fastfile it lacks, and the server
drops it when the file is not in the mod folder.

The decompiled test, '(char *)zoneFile + 1 != 0', is pointer arithmetic that
Clang and GCC may fold to true when optimizing: every zone then looked like a
mod zone, and no client could join a modded Linux server. This test lifts the
production assignment, compiles it with optimization, and feeds it a failed
and a successful open. Use --source with a pre-fix db_registry.cpp to see it
fail.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


HARNESS = r'''
#include <cstdint>
#include <cstdio>

// The handle comes from a function the optimizer cannot see through, as the
// real CreateFileA is in another unit.
__attribute__((noinline)) void *open_zone(int succeed) {
    static int handle;
    return succeed ? (void *)&handle : (void *)(intptr_t)-1;
}

__attribute__((noinline)) bool mod_zone_after_open(int succeed) {
    bool modZone = false;
    void *zoneFile = open_zone(succeed);
    MOD_ZONE_ASSIGNMENT
    return modZone;
}

int main(int argc, char **) {
    // argc is 1 at run time; written this way so the compiler cannot know it.
    const bool failed = mod_zone_after_open(argc - 1);
    const bool opened = mod_zone_after_open(argc);
    if (failed) {
        std::puts("FAIL: a failed open of the mod folder's zone counts as a mod zone");
        return 1;
    }
    if (!opened) {
        std::puts("FAIL: a zone opened from the mod folder does not count as a mod zone");
        return 1;
    }
    std::puts("OK: mod zone only when the mod folder's file opened");
    return 0;
}
'''


def assignment(source):
    match = re.search(r"^int32_t DB_TryLoadXFileInternal\(", source, re.MULTILINE)
    if not match:
        raise SystemExit("DB_TryLoadXFileInternal not found")
    body = source[match.end():]
    found = re.findall(r"^\s*(modZone = [^;\n]*zoneFile[^;\n]*;)", body[:body.find("\n}\n")], re.MULTILINE)
    if len(found) != 1:
        raise SystemExit(f"expected one modZone assignment from zoneFile, found {len(found)}")
    return found[0]


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source", type=Path, default=root / "src/database/db_registry.cpp")
    args = parser.parse_args()

    line = assignment(args.source.read_text(encoding="utf-8", errors="replace"))
    print(f"production line: {line}")
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        harness = Path(tmp) / "mod_zone.cpp"
        harness.write_text(HARNESS.replace("MOD_ZONE_ASSIGNMENT", line), encoding="utf-8")
        for level in ("-O0", "-O2"):
            binary = Path(tmp) / f"mod_zone{level}"
            subprocess.run(shlex.split(args.cxx) + ["-std=c++17", level, "-w", str(harness), "-o", str(binary)],
                           check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            print(f"{level}: {result.stdout.strip()}")
            failures += result.returncode != 0
    if failures:
        raise SystemExit(1)
    print("PASS: mod zone detection")


if __name__ == "__main__":
    main()
