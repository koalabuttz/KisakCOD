#!/usr/bin/env python3
"""Build the KisakCOD Android app (an APK) from source.

    python3 scripts/platform/android/build_apk.py --work /tmp/kisak-android [--config RelWithDebInfo]
        [--abi arm64-v8a|x86_64]

Needs the Android NDK (r27 or newer: --ndk, or $ANDROID_NDK_HOME) and the
Android SDK with platform 35 and build-tools (--sdk, or $ANDROID_HOME), CMake,
Ninja, git, Java 17 or newer and Gradle 8.13 or newer (--gradle, or `gradle`
on PATH). No game data is used or packed. See docs/android.md.

Steps, each kept in --work so a second run only redoes what changed:

  1. fetch and build the pinned SDL3 (libSDL3.so; its Java sources go into
     the app unchanged) and OpenAL Soft (libopenal.so) for the ABI;
  2. fetch the pinned DXVK Native headers (the D3D9 types the renderer
     compiles against) and build the D3D9 stand-in that answers "no device":
     Android has no DXVK, so the game draws through the Vulkan back end;
  3. build KisakCOD-mp as libmain.so and KisakCOD-sp (the campaign) as
     libmain_sp.so with KISAK_PLATFORM=android;
  4. stage the five libraries and run Gradle on scripts/platform/android,
     which writes <work>/KisakCOD-<config>.apk (debug-signed). The libraries
     keep their debug information for now, so a crash on a device can be
     read back to source lines (the APK is about 100 MB).

--abi x86_64 builds the app for the Android emulator on a PC instead of a
device (arm64-v8a, the default). Its builds sit beside the arm64 ones in
--work, with "-x86_64" added to their names, and the APK is
KisakCOD-<config>-x86_64.apk.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
ANDROID_DIR = ROOT / 'scripts/platform/android'

# Pinned sources. SDL3 matches the Linux client's (build-kisakcod-linux.yaml).
SDL3 = ('https://github.com/libsdl-org/SDL.git', '877399b2b2cf21e67554ed9046410f268ce1d1b2')  # 3.2.10
OPENAL = ('https://github.com/kcat/openal-soft.git', 'dc7d7054a5b4f3bec1dc23a42fd616a0847af948')  # 1.24.3
DXVK = ('https://github.com/doitsujin/dxvk.git', 'c3dd74be6baec53786d4e064a572185b70347a17')  # 2.7.1
DXVK_HEADERS = ('https://github.com/Joshua-Ashton/mingw-directx-headers.git',
                '9df86f2341616ef1888ae59919feaa6d4fad693d')
API = '29'
ABIS = {'arm64-v8a': 'aarch64', 'x86_64': 'x86_64'}  # Android ABI: clang target prefix
ABI = 'arm64-v8a'  # set by main from --abi


def run(*args, cwd=None):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def fetch(url_rev, dest):
    url, rev = url_rev
    if (dest / '.git').is_dir():
        head = subprocess.run(['git', '-C', str(dest), 'rev-parse', 'HEAD'],
                              capture_output=True, text=True).stdout.strip()
        if head == rev:
            return
    if dest.exists():
        shutil.rmtree(dest)
    run('git', 'init', '-q', dest)
    run('git', '-C', dest, 'remote', 'add', 'origin', url)
    run('git', '-C', dest, 'fetch', '-q', '--depth', '1', 'origin', rev)
    run('git', '-C', dest, 'checkout', '-q', '--detach', 'FETCH_HEAD')


def ndk_cmake(ndk, source, build, *extra):
    run('cmake', '-S', source, '-B', build, '-G', 'Ninja',
        f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake',
        f'-DANDROID_ABI={ABI}', f'-DANDROID_PLATFORM=android-{API}', '-DANDROID_STL=c++_static',
        '-DCMAKE_BUILD_TYPE=Release', '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON', *extra)
    run('cmake', '--build', build)


def ndk_clang(ndk):
    target = ABIS[ABI]
    found = sorted(Path(ndk).glob(f'toolchains/llvm/prebuilt/*/bin/{target}-linux-android{API}-clang++'))
    if not found:
        raise SystemExit(f'build_apk: no {target} clang++ for API {API} in {ndk}')
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--work', required=True, type=Path, help='folder for sources, builds and the APK')
    parser.add_argument('--config', default='RelWithDebInfo', choices=['Debug', 'RelWithDebInfo', 'Release'])
    parser.add_argument('--abi', default='arm64-v8a', choices=sorted(ABIS),
                        help='arm64-v8a for devices (default), x86_64 for the emulator on a PC')
    parser.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT'))
    parser.add_argument('--sdk', default=os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT'))
    parser.add_argument('--gradle', default='gradle')
    parser.add_argument('--gradle-arg', action='append', default=[],
                        help='one more argument for Gradle (repeatable), such as --init-script for a Maven mirror')
    parser.add_argument('--native-only', action='store_true',
                        help='stop after the native libraries (no Android SDK or Gradle needed)')
    args = parser.parse_args()
    if not args.ndk:
        parser.error('needs the NDK: --ndk or $ANDROID_NDK_HOME')
    if not args.native_only and not args.sdk:
        parser.error('needs the SDK: --sdk or $ANDROID_HOME (or --native-only)')
    global ABI
    ABI = args.abi
    # arm64 keeps the names it always had, so an existing --work is reused.
    suffix = '' if ABI == 'arm64-v8a' else f'-{ABI}'
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    ndk = Path(args.ndk).resolve()

    # 1. SDL3 and OpenAL Soft.
    fetch(SDL3, work / 'SDL3-source')
    sdl_sdk = work / f'SDL3-sdk{suffix}'
    if not (sdl_sdk / 'lib/libSDL3.so').is_file():
        ndk_cmake(ndk, work / 'SDL3-source', work / f'SDL3-build{suffix}', f'-DCMAKE_INSTALL_PREFIX={sdl_sdk}',
                  '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TESTS=OFF', '-DSDL_TEST_LIBRARY=OFF',
                  '-DSDL_EXAMPLES=OFF')
        run('cmake', '--install', work / f'SDL3-build{suffix}')
    fetch(OPENAL, work / 'openal-source')
    openal = work / f'openal-build{suffix}/libopenal.so'
    if not openal.is_file():
        ndk_cmake(ndk, work / 'openal-source', work / f'openal-build{suffix}', '-DALSOFT_UTILS=OFF',
                  '-DALSOFT_EXAMPLES=OFF', '-DALSOFT_TESTS=OFF', '-DALSOFT_INSTALL=OFF')

    # 2. DXVK Native's headers and the D3D9 stand-in.
    dxvk = work / 'dxvk-source'
    fetch(DXVK, dxvk)
    fetch(DXVK_HEADERS, dxvk / 'include/native/directx')
    dxvk_include = work / 'dxvk-include/dxvk'
    if not dxvk_include.is_dir():
        run(sys.executable, ROOT / 'scripts/tests/run_native_d3d9_boundary_checks.py',
            '--dxvk-source', dxvk, '--stage-dxvk-headers', dxvk_include)
    stand_in = work / f'stand-ins{suffix}/libd3d9_unavailable.so'
    stand_in.parent.mkdir(exist_ok=True)
    run(ndk_clang(ndk), '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
        '-Wl,-soname,libd3d9_unavailable.so', '-Wl,-z,max-page-size=16384', '-static-libstdc++',
        ROOT / 'tests/linux_device_probe/d3d9_unavailable.cpp', '-o', stand_in)

    # 3. libmain.so.
    build = work / f'build-{args.config}{suffix}'
    out = work / f'out-{args.config}{suffix}'
    run('cmake', '-S', ROOT, '-B', build, '-G', 'Ninja', '-DKISAK_PLATFORM=android', f'-DANDROID_NDK={ndk}',
        f'-DANDROID_ABI={ABI}',
        f'-DCMAKE_BUILD_TYPE={args.config}', f'-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={out}',
        f'-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={out}',
        '-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON', f'-DKISAK_DXVK_NATIVE_INCLUDE_DIR={dxvk_include}',
        f'-DKISAK_DXVK_NATIVE_LIBRARY={stand_in}',
        '-DKISAK_BUILD_LINUX_MP_CLIENT=ON', '-DKISAK_BUILD_LINUX_SP_GAME=ON', f'-DKISAK_OPENAL_LIBRARY={openal}',
        f'-DKISAK_OPENAL_INCLUDE_DIR={work / "openal-source/include"}',
        '-DKISAK_LINUX_CINEMATICS=OFF', '-DKISAK_LINUX_HTTP_DOWNLOADS=OFF', '-DKISAK_LINUX_GAMEPAD=ON',
        f'-DSDL3_DIR={sdl_sdk / "lib/cmake/SDL3"}',
        '-DKISAK_BUILD_VULKAN_PROBE=ON', '-DKISAK_BUILD_VULKAN_SHADERS=ON', '-DKISAK_BUILD_VULKAN_LEGACY=ON',
        '-DKISAK_BUILD_VULKAN_BACKEND=ON')
    run('cmake', '--build', build, '--target', 'KisakCOD-mp', 'KisakCOD-sp')
    libmain = out / 'libmain.so'
    libmain_sp = out / 'libmain_sp.so'

    # 4. The APK.
    jni = work / f'jniLibs{suffix}'
    (jni / ABI).mkdir(parents=True, exist_ok=True)
    for library in (libmain, libmain_sp, sdl_sdk / 'lib/libSDL3.so', openal, stand_in):
        shutil.copy2(library, jni / ABI / library.name)
    print(f'native libraries in {jni / ABI}')
    if args.native_only:
        return 0
    gradle_task = 'assembleDebug'
    env = dict(os.environ, ANDROID_HOME=str(Path(args.sdk).resolve()))
    print('+', args.gradle, gradle_task, flush=True)
    subprocess.run([args.gradle, '--no-daemon', '-p', str(ANDROID_DIR), gradle_task,
                    f'-PkisakJniLibs={jni}',
                    f'-PkisakSdlJava={work / "SDL3-source/android-project/app/src/main/java"}',
                    f'-PkisakVersionName={args.config}', f'-PkisakAbi={ABI}',
                    f'--project-cache-dir={work / "gradle-cache"}', *args.gradle_arg],
                   check=True, env=env)
    apk = ANDROID_DIR / 'app/build/outputs/apk/debug/app-debug.apk'
    final = work / f'KisakCOD-{args.config}{suffix}.apk'
    shutil.copy2(apk, final)
    print(f'APK: {final} ({final.stat().st_size // (1024 * 1024)} MB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
