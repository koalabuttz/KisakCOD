#!/usr/bin/env python3
"""Check an Android arm64 build of KisakCOD-dedi or libmain.so without running it.

    python3 scripts/tests/check_android_binary.py --ndk /path/to/android-ndk bin/KisakCOD-dedi
    python3 scripts/tests/check_android_binary.py --ndk /path/to/android-ndk --library libmain.so
    python3 scripts/tests/check_android_binary.py --self-test

No Android device or emulator is needed (docs/android.md). The checks read the
ELF file with the NDK's llvm-readelf and llvm-objdump:

  * it is a 64-bit Arm position-independent executable for Android's dynamic
    linker (/system/bin/linker64), as `adb shell` runs one;
  * it needs no shared library beyond bionic's libc, libm and libdl, so it runs
    when pushed alone to /data/local/tmp (the C++ library is linked in);
  * every loadable segment is aligned to at least 16 KiB, so it loads on
    devices with 16 KiB pages (Android 15 and later may use them);
  * it holds no fused multiply-add instruction (-ffp-contract=off): float
    results have to match x86's, as on Arm Linux (docs/linux-arm64.md).

With --library the file is libmain.so, the game client in the APK: a shared
object with no program interpreter that may also need the libraries packed
beside it (libSDL3.so, libopenal.so, libd3d9_unavailable.so) and Android's
liblog.so and libandroid.so, and exports
SDL_main, the one name SDL3's activity looks up, and nothing else.

--ndk defaults to $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT or $ANDROID_NDK.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ALLOWED_LIBRARIES = {'libc.so', 'libm.so', 'libdl.so'}
# liblog.so and libandroid.so are Android's own: the client copies its console
# to logcat, and the Vulkan host takes the activity's window from its Surface
# (ANativeWindow_fromSurface) when Android gives it a new one.
APP_LIBRARIES = {'libSDL3.so', 'libopenal.so', 'libd3d9_unavailable.so', 'liblog.so', 'libandroid.so'}
APP_EXPORTS = ['SDL_main']
INTERPRETER = '/system/bin/linker64'
MIN_ALIGNMENT = 16 * 1024
FUSED = re.compile(r'\t(fmadd|fmsub|fnmadd|fnmsub|fmla|fmls)\b')


def ndk_tool(ndk, name):
    prebuilt = Path(ndk) / 'toolchains/llvm/prebuilt'
    hosts = sorted(prebuilt.glob('*/bin/' + name)) if prebuilt.is_dir() else []
    if not hosts:
        raise SystemExit(f'check_android_binary: no {name} under {prebuilt}')
    return str(hosts[0])


def problems(headers, dynamic, disassembly, library=False, exports=None):
    """The checks on the tools' output; returns a list of problems."""
    found = []
    if not re.search(r'Class:\s+ELF64', headers):
        found.append('not a 64-bit ELF file')
    if not re.search(r'Machine:\s+AArch64', headers):
        found.append('not built for AArch64')
    if not re.search(r'Type:\s+DYN', headers):
        found.append('not a position-independent executable')
    interpreter = re.search(r'Requesting program interpreter: ([^\]]+)\]', headers)
    if library:
        if interpreter:
            found.append(f'a library should have no program interpreter, not {interpreter.group(1)}')
        if exports != APP_EXPORTS:
            found.append(f'exports {exports}, not only {APP_EXPORTS}')
    elif not interpreter or interpreter.group(1) != INTERPRETER:
        found.append(f'program interpreter is {interpreter.group(1) if interpreter else "missing"}, '
                     f'not {INTERPRETER}')
    # The alignment is the last field; the flags before it may hold a space ("R E").
    loads = re.findall(r'^\s*LOAD\s.*\s(0x[0-9a-f]+)\s*$', headers, re.M)
    if not loads:
        found.append('no loadable segments found')
    for alignment in loads:
        if int(alignment, 16) < MIN_ALIGNMENT:
            found.append(f'a loadable segment is aligned to {alignment}, under 16 KiB')
    needed = set(re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', dynamic))
    extra = sorted(needed - ALLOWED_LIBRARIES - (APP_LIBRARIES if library else set()))
    if extra:
        found.append('needs shared libraries beyond bionic: ' + ', '.join(extra))
    fused = FUSED.findall(disassembly)
    if fused:
        found.append(f'{len(fused)} fused multiply-add instructions ({", ".join(sorted(set(fused)))})')
    return found


def check(ndk, binary, library=False):
    readelf = ndk_tool(ndk, 'llvm-readelf')
    objdump = ndk_tool(ndk, 'llvm-objdump')
    run = lambda *args: subprocess.run(args, check=True, capture_output=True, text=True).stdout
    headers = run(readelf, '--file-header', '--program-headers', '--wide', binary)
    dynamic = run(readelf, '--dynamic', binary)
    disassembly = run(objdump, '-d', '--no-show-raw-insn', binary)
    exports = None
    if library:
        nm = ndk_tool(ndk, 'llvm-nm')
        exports = sorted(line.split()[-1] for line in run(nm, '-D', '--defined-only', binary).splitlines()
                         if line.strip())
    return problems(headers, dynamic, disassembly, library, exports)


HEADERS_OK = """ELF Header:
  Class:                             ELF64
  Type:                              DYN (Shared object file)
  Machine:                           AArch64
Program Headers:
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  PHDR           0x000040 0x0000000000000040 0x0000000000000040 0x000268 0x000268 R   0x8
  INTERP         0x0002a8 0x00000000000002a8 0x00000000000002a8 0x000015 0x000015 R   0x1
      [Requesting program interpreter: /system/bin/linker64]
  LOAD           0x000000 0x0000000000000000 0x0000000000000000 0x28b320 0x28b320 R E 0x4000
  LOAD           0x28b340 0x000000000028f340 0x000000000028f340 0x0186a8 0x018cc0 RW  0x4000
"""
DYNAMIC_OK = """  0x0000000000000001 (NEEDED)             Shared library: [libdl.so]
  0x0000000000000001 (NEEDED)             Shared library: [libc.so]
"""
DISASSEMBLY_OK = "  4000:\tfmul\ts0, s0, s1\n  4004:\tfadd\ts0, s0, s2\n"


def self_test():
    assert problems(HEADERS_OK, DYNAMIC_OK, DISASSEMBLY_OK) == [], problems(HEADERS_OK, DYNAMIC_OK, DISASSEMBLY_OK)
    glibc = HEADERS_OK.replace(INTERPRETER, '/lib/ld-linux-aarch64.so.1')
    assert any('interpreter' in p for p in problems(glibc, DYNAMIC_OK, DISASSEMBLY_OK))
    four_k = HEADERS_OK.replace('R E 0x4000', 'R E 0x1000')
    assert any('16 KiB' in p for p in problems(four_k, DYNAMIC_OK, DISASSEMBLY_OK))
    shared_cxx = DYNAMIC_OK + '  0x0000000000000001 (NEEDED)             Shared library: [libc++_shared.so]\n'
    assert any('libc++_shared.so' in p for p in problems(HEADERS_OK, shared_cxx, DISASSEMBLY_OK))
    fused = DISASSEMBLY_OK + '  4008:\tfmadd\ts0, s1, s2, s3\n'
    assert any('fused' in p for p in problems(HEADERS_OK, DYNAMIC_OK, fused))
    x86 = HEADERS_OK.replace('AArch64', 'Advanced Micro Devices X86-64')
    assert any('AArch64' in p for p in problems(x86, DYNAMIC_OK, DISASSEMBLY_OK))
    # libmain.so: no interpreter, the app's own libraries allowed, SDL_main only.
    library = HEADERS_OK.replace('      [Requesting program interpreter: /system/bin/linker64]\n', '')
    app = DYNAMIC_OK + '  0x0000000000000001 (NEEDED)             Shared library: [libSDL3.so]\n'
    assert problems(library, app, DISASSEMBLY_OK, True, ['SDL_main']) == []
    assert any('libSDL3.so' in p for p in problems(HEADERS_OK, app, DISASSEMBLY_OK))
    assert any('exports' in p for p in problems(library, app, DISASSEMBLY_OK, True, ['SDL_main', 'inflate']))
    assert any('interpreter' in p for p in problems(HEADERS_OK, app, DISASSEMBLY_OK, True, ['SDL_main']))
    print('check_android_binary self-test: ok')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('binary', nargs='?')
    parser.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
                        or os.environ.get('ANDROID_NDK'))
    parser.add_argument('--library', action='store_true', help='the file is libmain.so, not the server')
    parser.add_argument('--self-test', action='store_true', help='check the checks against sample output and exit')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.binary or not args.ndk:
        parser.error('needs the binary and the NDK (--ndk or $ANDROID_NDK_HOME)')
    found = check(args.ndk, args.binary, args.library)
    for problem in found:
        print(f'{args.binary}: {problem}', file=sys.stderr)
    if found:
        return 1
    kind = 'library exporting SDL_main only' if args.library else 'executable, bionic only'
    print(f'{args.binary}: Android arm64 {kind}, 16 KiB segments, no fused multiply-add')
    return 0


if __name__ == '__main__':
    sys.exit(main())
