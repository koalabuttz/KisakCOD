# Android track

An opt-in Android build, started 2026-11-15. The target device is a handheld
with built-in controls, the Retroid Pocket 6 (Snapdragon 8 Gen 2: Kryo CPU,
Adreno 740 GPU with Vulkan; Android 13; 1920x1080 at up to 120 Hz), so the
port is controller first. The game's files come from the player's own copy of
Call of Duty 4; nothing of the game is ever shipped.

Android rides on two tracks that already exist:

- the **aarch64 Linux track** ([linux-arm64.md](linux-arm64.md)): the 64-bit
  fixes, signed `char`, no fused multiply-add, Arm intrinsics and pages larger
  than 4 KiB. Android's C library (bionic) is a Linux C library, so the POSIX
  layer in `src/linux` builds as it does for Arm Linux;
- the **Vulkan back end** ([vulkan-d3d9-front.md](vulkan-d3d9-front.md)): the
  renderer's Direct3D 9 calls drawn through KisakCOD's own Vulkan core, with
  SDL3 for the window. Android has no DXVK, so the game will run with
  `r_backend vulkan` only.

Nothing on this page changes the default build or any other track.

## Stages

1. **Dedicated server** (this page, done): `KisakCOD-dedi` builds for
   arm64-v8a with the Android NDK and passes the binary checks below in
   hosted CI. It has not run on a device yet; the first device check is to
   run it with `adb` and load a stock map.
2. **The game as an app**: `KisakCOD-mp` built as the native library of an
   SDL3 app (an APK), with the Vulkan back end, the D3D9 stand-in in place of
   DXVK and OpenAL Soft for sound. CI builds the APK without game data.
3. **First frame on the handheld**: the menu drawn through Vulkan on the
   Adreno driver, the built-in controls as an SDL gamepad (the Linux client's
   gamepad support, `KISAK_LINUX_GAMEPAD`), the touch screen as the menu
   pointer, the Back button as Escape.
4. **A whole match**: sound, the app going to the background and back (the
   window surface is lost and made again), the screen kept on, a local match
   with bots and a LAN or internet server.
5. **Speed**: 60 frames a second at 1080p with Legacy visuals, measured on
   the handheld; tile-friendly render passes for the Adreno GPU, the shader
   cache, heat.
6. **For players**: choosing the game folder in the app, movies (FFmpeg),
   optional on-screen touch controls for phones, a signed APK from CI.

## Building the dedicated server

Needs the Android NDK r27 or newer (GitHub's Ubuntu runners have one at
`$ANDROID_NDK_HOME`), CMake and Ninja. The host compiler is not used.

```sh
cmake -S . -B build-android -G Ninja \
    -DKISAK_PLATFORM=android \
    -DANDROID_NDK=/path/to/android-ndk \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-android --target KisakCOD-dedi
```

- `-DANDROID_NDK` can be left out when `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`
  or `ANDROID_NDK` is set. `scripts/platform/android/android_toolchain.cmake`
  picks the NDK's own toolchain file before `project()`; a
  `-DCMAKE_TOOLCHAIN_FILE` on the command line is used as it is.
- `ANDROID_ABI` is `arm64-v8a`, the only ABI offered. `ANDROID_PLATFORM`
  defaults to `android-29` (Android 10). `ANDROID_STL` defaults to
  `c++_static`, so the server is one file that runs without a
  `libc++_shared.so` beside it.
- `KISAK_LINUX_ARCH` is set to `aarch64`, so every rule of the Arm Linux build
  applies. `-fsigned-char -ffp-contract=off` are added as compile options in
  `scripts/platform/linux/platform.cmake`, because the NDK's toolchain file
  replaces the `CMAKE_<LANG>_FLAGS_INIT` values the Linux tracks use.
- Every other target and opt-in (the client, the probes, Steam, Tracy) is
  refused with `KISAK_PLATFORM=android` for now.

The source files built are the Linux server's, unmodified: no engine source
needed a change for bionic at this stage.

## Checks without a device

```sh
python3 scripts/tests/check_android_binary.py --ndk /path/to/android-ndk bin/KisakCOD-dedi
python3 scripts/tests/check_android_binary.py --self-test
```

The check reads the ELF file with the NDK's `llvm-readelf` and `llvm-objdump`:
a 64-bit Arm position-independent executable for `/system/bin/linker64`, no
shared library beyond bionic's `libc.so`, `libm.so` and `libdl.so`, every
loadable segment aligned to 16 KiB (Android 15 and later can use 16 KiB
pages), and no fused multiply-add instruction.

The binary cannot run in hosted CI or under `qemu-aarch64`: it needs Android's
dynamic linker and system libraries. Hosted CI: the **Android** workflow
(`.github/workflows/build-kisakcod-android.yaml`) builds Debug and
RelWithDebInfo, runs the check on both, and keeps the RelWithDebInfo server as
the artifact `KisakCOD-dedi-android-arm64-RelWithDebInfo` for device runs.

## Running the server on a device

With USB debugging on and the device connected (`adb devices` lists it), and a
copy of the game folder from your own install (`main`, `zone` and the rest) on
the device's shared storage, for example `/sdcard/KisakCOD/cod4`:

```sh
adb push KisakCOD-dedi /data/local/tmp/
adb shell chmod 755 /data/local/tmp/KisakCOD-dedi
adb shell 'cd /data/local/tmp && ./KisakCOD-dedi \
    +set fs_basepath /sdcard/KisakCOD/cod4 +set fs_homepath /data/local/tmp/kisak \
    +set dedicated 1 +map mp_crash'
```

`/data/local/tmp` is the one place `adb shell` may run a program from;
`fs_homepath` points there too because the shell cannot write to the game
folder's parent on every device. This has not been tried on a device yet.
