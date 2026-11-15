#!/usr/bin/env python3
"""Build the KisakCOD Android app (an APK) from source.

    python3 scripts/platform/android/build_apk.py --work /tmp/kisak-android [--config RelWithDebInfo]

Needs the Android NDK (r27 or newer: --ndk, or $ANDROID_NDK_HOME) and the
Android SDK with platform 35 and build-tools (--sdk, or $ANDROID_HOME), CMake,
Ninja, git, Java 17 or newer and Gradle 8.13 or newer (--gradle, or `gradle`
on PATH). No game data is used or packed. See docs/android.md.

Steps, each kept in --work so a second run only redoes what changed:

  1. fetch and build the pinned SDL3 (libSDL3.so; its Java sources go into
     the app unchanged) and OpenAL Soft (libopenal.so) for arm64-v8a;
  2. fetch the pinned DXVK Native headers (the D3D9 types the renderer
     compiles against) and build the D3D9 stand-in that answers "no device":
     Android has no DXVK, so the game draws through the Vulkan back end;
  3. build KisakCOD-mp as libmain.so with KISAK_PLATFORM=android;
  4. stage the four libraries and run Gradle on scripts/platform/android,
     which writes <work>/KisakCOD-<config>.apk (debug-signed).
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
ABI = 'arm64-v8a'


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
        '-DCMAKE_BUILD_TYPE=Release', *extra)
    run('cmake', '--build', build)


def ndk_clang(ndk):
    found = sorted(Path(ndk).glob(f'toolchains/llvm/prebuilt/*/bin/aarch64-linux-android{API}-clang++'))
    if not found:
        raise SystemExit(f'build_apk: no aarch64 clang++ for API {API} in {ndk}')
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--work', required=True, type=Path, help='folder for sources, builds and the APK')
    parser.add_argument('--config', default='RelWithDebInfo', choices=['Debug', 'RelWithDebInfo', 'Release'])
    parser.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT'))
    parser.add_argument('--sdk', default=os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT'))
    parser.add_argument('--gradle', default='gradle')
    parser.add_argument('--native-only', action='store_true',
                        help='stop after the native libraries (no Android SDK or Gradle needed)')
    args = parser.parse_args()
    if not args.ndk:
        parser.error('needs the NDK: --ndk or $ANDROID_NDK_HOME')
    if not args.native_only and not args.sdk:
        parser.error('needs the SDK: --sdk or $ANDROID_HOME (or --native-only)')
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    ndk = Path(args.ndk).resolve()

    # 1. SDL3 and OpenAL Soft.
    fetch(SDL3, work / 'SDL3-source')
    sdl_sdk = work / 'SDL3-sdk'
    if not (sdl_sdk / 'lib/libSDL3.so').is_file():
        ndk_cmake(ndk, work / 'SDL3-source', work / 'SDL3-build', f'-DCMAKE_INSTALL_PREFIX={sdl_sdk}',
                  '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TESTS=OFF', '-DSDL_TEST_LIBRARY=OFF',
                  '-DSDL_EXAMPLES=OFF')
        run('cmake', '--install', work / 'SDL3-build')
    fetch(OPENAL, work / 'openal-source')
    openal = work / 'openal-build/libopenal.so'
    if not openal.is_file():
        ndk_cmake(ndk, work / 'openal-source', work / 'openal-build', '-DALSOFT_UTILS=OFF',
                  '-DALSOFT_EXAMPLES=OFF', '-DALSOFT_TESTS=OFF', '-DALSOFT_INSTALL=OFF')

    # 2. DXVK Native's headers and the D3D9 stand-in.
    dxvk = work / 'dxvk-source'
    fetch(DXVK, dxvk)
    fetch(DXVK_HEADERS, dxvk / 'include/native/directx')
    dxvk_include = work / 'dxvk-include/dxvk'
    if not dxvk_include.is_dir():
        run(sys.executable, ROOT / 'scripts/tests/run_native_d3d9_boundary_checks.py',
            '--dxvk-source', dxvk, '--stage-dxvk-headers', dxvk_include)
    stand_in = work / 'stand-ins/libd3d9_unavailable.so'
    stand_in.parent.mkdir(exist_ok=True)
    run(ndk_clang(ndk), '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
        '-Wl,-soname,libd3d9_unavailable.so', '-Wl,-z,max-page-size=16384', '-static-libstdc++',
        ROOT / 'tests/linux_device_probe/d3d9_unavailable.cpp', '-o', stand_in)

    # 3. libmain.so.
    build = work / f'build-{args.config}'
    out = work / f'out-{args.config}'
    run('cmake', '-S', ROOT, '-B', build, '-G', 'Ninja', '-DKISAK_PLATFORM=android', f'-DANDROID_NDK={ndk}',
        f'-DCMAKE_BUILD_TYPE={args.config}', f'-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={out}',
        f'-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={out}',
        '-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON', f'-DKISAK_DXVK_NATIVE_INCLUDE_DIR={dxvk_include}',
        f'-DKISAK_DXVK_NATIVE_LIBRARY={stand_in}',
        '-DKISAK_BUILD_LINUX_MP_CLIENT=ON', f'-DKISAK_OPENAL_LIBRARY={openal}',
        f'-DKISAK_OPENAL_INCLUDE_DIR={work / "openal-source/include"}',
        '-DKISAK_LINUX_CINEMATICS=OFF', '-DKISAK_LINUX_HTTP_DOWNLOADS=OFF', '-DKISAK_LINUX_GAMEPAD=ON',
        f'-DSDL3_DIR={sdl_sdk / "lib/cmake/SDL3"}',
        '-DKISAK_BUILD_VULKAN_PROBE=ON', '-DKISAK_BUILD_VULKAN_SHADERS=ON', '-DKISAK_BUILD_VULKAN_LEGACY=ON',
        '-DKISAK_BUILD_VULKAN_BACKEND=ON')
    run('cmake', '--build', build, '--target', 'KisakCOD-mp')
    libmain = out / 'libmain.so'

    # 4. The APK.
    jni = work / 'jniLibs'
    (jni / ABI).mkdir(parents=True, exist_ok=True)
    for library in (libmain, sdl_sdk / 'lib/libSDL3.so', openal, stand_in):
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
                    f'-PkisakVersionName={args.config}',
                    f'-Pandroid.ndkPath={ndk}',
                    f'--project-cache-dir={work / "gradle-cache"}'],
                   check=True, env=env)
    apk = ANDROID_DIR / 'app/build/outputs/apk/debug/app-debug.apk'
    final = work / f'KisakCOD-{args.config}.apk'
    shutil.copy2(apk, final)
    print(f'APK: {final} ({final.stat().st_size // (1024 * 1024)} MB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
