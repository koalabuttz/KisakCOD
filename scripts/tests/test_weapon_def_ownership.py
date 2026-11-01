#!/usr/bin/env python3
"""Exercise production weapon load/shutdown ownership without game assets.

Fastfile definitions borrow database-owned strings. Raw definitions acquire
individual references. Run both client and dedicated preprocessor paths, with
and without fs_game/useFastFile, including repeated shutdown and map reuse.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_random_range import block

ROOT = Path(__file__).resolve().parents[2]
PREAMBLE = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#define __cdecl
#define iassert assert
using _BYTE = unsigned char;
struct WeaponDef {
    uint16_t hideTags[8]{};
    uint16_t notetrackSoundMapKeys[16]{};
    uint16_t notetrackSoundMapValues[16]{};
};
struct Dvar { union { const char *string; intptr_t integer; } current; } gameDir;
Dvar *fs_gameDirVar = &gameDir;
bool fastfile;
bool IsFastFileLoad() { return fastfile; }
unsigned bg_lastParsedWeaponIndex;
WeaponDef *bg_weaponDefs[4];
WeaponDef raw[2], database[2];
int loaded, rawLoads, databaseLoads, soundClears;
std::map<unsigned, int> references;
void SL_RemoveRefToString(unsigned id) {
    // Database strings have no individually owned reference to release.
    assert(id < 100);
    assert(references.at(id) > 0);
    --references.at(id);
}
void BG_ClearSurfaceTypeSounds() { ++soundClears; }
WeaponDef *BG_LoadWeaponDef_LoadObj(const char *) { ++rawLoads; return &raw[loaded++]; }
WeaponDef *BG_LoadWeaponDef_FastFile(const char *) { ++databaseLoads; return &database[loaded++]; }
'''
TEST = r'''
int main() {
    for (bool mod : {false, true}) for (bool ff : {false, true}) {
        gameDir.current.string = mod ? "mods/test" : "";
        fastfile = ff;
#ifdef DEDICATED
        const bool owned = false;
#else
        const bool owned = mod || !ff;
#endif
        soundClears = 0;
        for (int cycle = 0; cycle != 10; ++cycle) {
            loaded = rawLoads = databaseLoads = 0;
            references.clear();
            // Populate every array boundary and include shared and null strings.
            for (int i = 0; i != 2; ++i) {
                raw[i] = {}; database[i] = {};
                for (int j = 0; j != 8; ++j) {
                    raw[i].hideTags[j] = j % 2 ? 1 : 0;
                    database[i].hideTags[j] = j % 2 ? 101 : 0;
                    if (j % 2) ++references[1];
                }
                for (int j = 0; j != 16; ++j) {
                    raw[i].notetrackSoundMapKeys[j] = 2;
                    raw[i].notetrackSoundMapValues[j] = 3;
                    database[i].notetrackSoundMapKeys[j] = 102;
                    database[i].notetrackSoundMapValues[j] = 103;
                    ++references[2]; ++references[3];
                }
                bg_weaponDefs[++bg_lastParsedWeaponIndex] = BG_LoadWeaponDef("weapon");
            }
            assert(rawLoads == (owned ? 2 : 0));
            assert(databaseLoads == (owned ? 0 : 2));
            auto before = references;
            BG_ShutdownWeaponDefFiles();
            assert(bg_lastParsedWeaponIndex == 0);
            for (auto [id, count] : references) assert(count == (owned ? 0 : before[id]));
            assert(soundClears == (owned ? cycle * 2 + 1 : 0));
            BG_ShutdownWeaponDefFiles(); // no double release after resetting the registry
            assert(bg_lastParsedWeaponIndex == 0);
        }
    }
    puts("PASS: 40 load/shutdown cycles, repeated teardown, mod/fastfile ownership matrix");
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'clang++'))
    parser.add_argument('--source', type=Path, default=ROOT / 'src/bgame/bg_weapons.cpp')
    parser.add_argument('--m32', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = args.source.read_text()
    bodies = '\n'.join(block(source, source.index(signature)) for signature in (
        'void __cdecl BG_FreeWeaponDefStrings()',
        'void __cdecl BG_ShutdownWeaponDefFiles()',
        'WeaponDef *__cdecl BG_LoadWeaponDef(const char *name)',
    ))
    with tempfile.TemporaryDirectory(prefix='kisak-weapon-ownership-') as directory:
        work = Path(directory)
        unit = work / 'ownership.cpp'
        unit.write_text(PREAMBLE + bodies + TEST)
        for dedicated in (True, False):
            binary = work / ('dedicated' if dedicated else 'client')
            flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-g']
            if dedicated: flags += ['-DDEDICATED']
            if args.m32: flags += ['-m32']
            if args.sanitize: flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
            subprocess.run(shlex.split(args.cxx) + flags + [str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
