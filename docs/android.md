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
2. **The game as an app** (builds; see [below](#building-the-app)):
   `KisakCOD-mp` built as `libmain.so`, the native library of an SDL3 app
   (an APK), with the Vulkan back end, the D3D9 stand-in in place of DXVK
   and OpenAL Soft for sound. CI builds the APK without game data. Not
   installed on a device yet.
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

## Building the app

```sh
python3 scripts/platform/android/build_apk.py --work /tmp/kisak-android \
    --ndk /path/to/android-ndk --sdk /path/to/android-sdk
```

writes `/tmp/kisak-android/KisakCOD-RelWithDebInfo.apk`, signed with Gradle's
debug key. It needs the NDK, the Android SDK (platform 35 and build-tools),
CMake, Ninja, git, Java 17 or newer and Gradle 8.13 or newer (`--gradle`, or
`gradle` on `PATH`); `--native-only` stops before the SDK and Gradle are
needed. Everything it fetches is pinned in the script:

| Piece | Version | What for |
| --- | --- | --- |
| SDL3 | 3.2.10 (`877399b2`), as the Linux client | Window, input, the gamepad API, and its Java activity (`org.libsdl.app`), compiled into the app unchanged |
| OpenAL Soft | 1.24.3 (`dc7d7054`) | Sound (its OpenSL ES output) |
| DXVK Native headers | 2.7.1 (`c3dd74be`), headers only | The D3D9 types the renderer compiles against |

The libraries in the APK:

| Library | From |
| --- | --- |
| `libmain.so` | `KisakCOD-mp` with `KISAK_PLATFORM=android`: a shared library, not a program, exporting only `SDL_main` (`scripts/platform/android/main_exports.map`) |
| `libSDL3.so` | SDL3 |
| `libopenal.so` | OpenAL Soft |
| `libd3d9_unavailable.so` | `tests/linux_device_probe/d3d9_unavailable.cpp`: answers "no device" to anything that asks the system's D3D9 |

What is Android-specific in the client:

- `src/linux/main.cpp` names its `main` `SDL_main` on Android (C linkage,
  exported), as SDL3's `SDL_main.h` would: SDL3's activity loads
  `libmain.so` and calls it once the window exists.
- `r_backend` defaults to `vulkan` on Android
  (`src/linux/client/vulkan/vulkan_backend.cpp`). `d3d9` reaches only the
  stand-in.
- The configuration refuses `KisakCOD-mp` without the Vulkan back end
  (`KISAK_BUILD_VULKAN_PROBE`, `_SHADERS`, `_LEGACY` and `_BACKEND`, which
  the script turns on), and builds the controller support
  (`KISAK_LINUX_GAMEPAD`, [linux-mp-client.md](linux-mp-client.md)).
- `scripts/platform/android/app/.../KisakActivity.java` extends SDL3's
  activity: it loads `libSDL3.so` and `libmain.so` and starts the game with
  `+set fs_basepath <app folder> +set fs_homepath <app folder>/home`, where
  the app folder is `Android/data/org.kisakcod.game/files` on shared
  storage. The app needs no storage permission for its own folder.
- The manifest asks for Vulkan 1.3, keeps the app in landscape and full
  screen, and asks for network access (multiplayer).

`check_android_binary.py --library libmain.so` checks the client library the
way it checks the server, except that a library has no program interpreter,
may need the three libraries above, and must export `SDL_main` alone.

`build_apk.py --abi x86_64` builds the app for the Android emulator on a PC
instead (`KisakCOD-<config>-x86_64.apk`; its builds sit beside the arm64 ones
in `--work`, named with `-x86_64`). It takes the x86_64 Linux track's code. No
device is x86, and the emulator draws through the PC's own GPU driver, so it
finds no Adreno-only fault; it is for runs nobody has to watch
(`scripts/runs/android_run.py`).

### The first screen

The app opens on a screen of big buttons (`LauncherActivity.java`) that the
controller works: the d-pad or left stick moves, A presses, L1 and R1 change
the number of bots. It lists every map in the game folder (`zone/*/mp_*.ff`
and `usermaps`); a map starts a match against KisakCOD's bots
(`+set developer_script 1 +set bot_brain 1 +set scr_testclients <n> +set
g_gametype war|dm +devmap <map>`), "Multiplayer" starts the multiplayer
game's own menu and "Campaign" the singleplayer game's. "Visuals" opens a
page of its own (`VisualsActivity.java`) with a button for each setting of
the look and a Preset button that sets them together: Legacy (`+set
r_visuals legacy`, 60 fps), Modern (`+set r_aaSamples 1 +set r_visuals
modern`, docs/vulkan-modern.md, every pass at the renderer's default and god
rays on, held to an even 30 fps with `r_vsync 1`, `r_vsyncInterval 2`) or
Modern Fast (Modern at 60 fps with ambient occlusion, screen-space
reflections and contact shadows off). Changing any setting makes the preset
Custom. The settings: the look; the frame rate (60 fps, 30 fps or no
vsync); the sun's shadows (`sm_enable`, off by default, see below); ambient
occlusion (default, on, half resolution or off); screen-space reflections,
contact shadows, soft sun and lamp shadows, volumetric fog, fog lit by
lamps, god rays, bloom and auto exposure (each default, on or off, the
`r_modern*` switches); mirror (planar) reflections, live reflection probes,
glowing lamps, muzzle-flash light and the water shader (each default, on or
off, as `KISAK_VK_MODERN_*` variables in the start's env extra, since they
have no dvar); and the tone map. Modern's settings show dimmed while the look
is Legacy. On the Retroid Pocket 6, Modern with everything on drew Crash in
about 30 ms (33 fps); ambient occlusion cost about 6 ms and reflections about
5; Modern Fast runs at 60 fps (GPU frame about 15 ms). The look, the sun's
shadows and the variables go with every start; the game's saved settings
(frame rate, the `r_modern*` switches, the tone map) only when they changed
since the last start, so what the game's settings menu changes later stays.
The sun's shadows are off on Android unless the Visuals page turns them on
(`KisakActivity` passes `sm_enable 0` first), so the fog shows no light
shafts there by default. The game runs in a
process of its own (`:game`), which ends with the game: the engine never
starts twice in one process, and the first screen comes back when the game
quits.

Quitting ends the game's process with `_exit(0)` (`Sys_Quit`, `src/linux/main.cpp`): a plain `exit()` ran Android's UI libraries' exit handlers while the activity's render thread still drew, and every quit ended in SIGABRT in an hwui thread.

### The campaign

The singleplayer game, KisakCOD-sp, builds as `libmain_sp.so` beside
`libmain.so` (`sp_game.cmake`, `build_apk.py`), through the same Vulkan back
end. CampaignActivity (KisakActivity in the same `:game` process) loads it
instead of `libmain.so`, as KisakActivity does when the intent's boolean extra
`campaign` is true; runs that can only name an activity use the first:

```
adb shell am start -n org.kisakcod.game/.CampaignActivity --es args "+devmap killhouse"
```

A level needs an active player profile ("Tried to use a player profile before
it was set"), and a handheld has no keyboard to name one, so the first
campaign start makes one called `player` (`home/players/profiles/active.txt`)
unless the home folder has one. The controller works as in multiplayer
(`KISAK_LINUX_GAMEPAD`: `mp_client_gamepad.cpp` serves both games). It has
not run on a device or the emulator yet.

The stock scripts read `scr_testclients` only in their developer blocks
(`/# ... #/`), so without `developer_script 1` no bot joins (run 6 on the
Pocket 6: the dvar read back as 9 and nobody came). `devmap` is what the PC's
bot runs use (`scripts/runs/recipes/vulkan-ab-mp-crash.json`).

### Installing and starting the app

Install, start once (the first start makes the app's folder), copy the game
folder from your own install (`main`, `zone` and the rest) into it, and start
again:

```sh
adb install -r KisakCOD-RelWithDebInfo.apk
adb shell am start -n org.kisakcod.game/.KisakActivity
adb shell am force-stop org.kisakcod.game
adb push "Call of Duty 4/." /sdcard/Android/data/org.kisakcod.game/files/
adb shell am start -n org.kisakcod.game/.KisakActivity
adb logcat -s KisakCOD SDL
```

Extra engine arguments for a run go in the `args` extra, split at spaces:
`adb shell am start -n org.kisakcod.game/.KisakActivity --es args "+set developer 1"`.
`KISAK_*` environment variables (the start-up knobs such as
`KISAK_VK_MODERN_SSR_STEPS`) go in the `env` extra as `NAME=VALUE` pairs split
at spaces, and the activity sets them in its process before the game starts:
`--es env "KISAK_VK_MODERN_SSR_STEPS=16 KISAK_VK_MODERN_AO_HALF=0"`. Other
names are refused. `scripts/runs/android_run.py` fills it from a recipe's or
launch's `env`.
The activity starts the game without shadow maps (`+set sm_enable 0`): on
the Adreno 740 the sun's shadows flickered from frame to frame (2026-11-15,
cause not found yet). The activity always starts the game full screen (`+set r_fullscreen 1`, so
SDL hides Android's system bars; run 3 had a 1920x970 window under the
navigation bar). For an even 30 frames a second (Modern visuals on the Retroid Pocket 6
run at 33-40), `+set r_vsync 1 +set r_vsyncInterval 2` shows each frame for
two refreshes of a 60 Hz screen (four at 120 Hz: `r_vsyncInterval 4`), paced
through Android's `VK_GOOGLE_display_timing` (docs/vulkan-core.md, Swapchain).
The game's console goes to logcat under the tag KisakCOD: Android drops an
app's stdout and stderr, so `src/linux/main.cpp` copies them there.

First start on the Retroid Pocket 6 (2026-11-15, CI APK of 91d7799): the menu
drew with Vulkan, but turned a quarter and squeezed. The panel's own
orientation is portrait, so the surface reports a quarter-turn transform while
the game runs in landscape, and the swapchain passed that transform on: the
compositor took the landscape picture as already turned. The swapchain now
asks for no rotation (`ChoosePreTransform` in `src/gfx_vk/vk_core.cpp`), the
compositor turns the picture, and the `VK_SUBOPTIMAL_KHR` that Android
reports for that alone does not rebuild the swapchain. Drawing the picture
turned (Android's pre-rotation) would save the compositor that work; it is
left for the speed stage. Android tells such a swapchain nothing when the
window changes size (it goes full screen just after the game starts) and
scales the old picture, so every 32nd frame compares the surface's size with
the swapchain's and rebuilds it when they differ (runs 4 and 5 kept a
1920x970 swapchain on the 1920x1080 screen).

Android takes the window away from an app sent to the background (Home, the
lock screen) and gives it a new one when it comes back; SDL meanwhile holds
the game's main thread in its event loop. Presents, acquires and size queries
on the old window answer `VK_ERROR_SURFACE_LOST_KHR`, which no longer fails
the core: the next frame lets go of the swapchain and the surface and asks
the window for a new surface (`RenewSurface` in `src/gfx_vk/vk_core.cpp`),
skipping frames until there is one, and builds a swapchain on it. The Vulkan
window probe's `--lose-surface` (in `test_vulkan_window_probe.py`) does the
same on a desktop window. On Android the client makes the surface itself
from the activity's `Surface` as Java has it at that moment
(`AndroidWindowNow` in `src/linux/client/vulkan/vulkan_backend.cpp`): SDL's
copy of the window goes on the Java thread while the render thread may be
asking, and with the screen turned off SDL handed Vulkan a null window
(run 7, a crash in `vkCreateAndroidSurfaceKHR`).

The "computer changed, set optimal settings?" question is not asked on
Android: a handheld measures a different CPU speed at nearly every start, and
the box came up over a black screen (run 7).

What run 4 (mp_crash on the Pocket 6) showed, and the fixes:

- Sticks and triggers did nothing while the buttons worked. The window host
  started SDL's text input for the window's whole life, as on a desktop; on
  Android that brings up the on-screen keyboard and moves input focus to
  SDL's hidden text field, which gets key events but not the controller's
  motion. Where typing needs an on-screen keyboard
  (`SDL_HasScreenKeyboardSupport`), text input now runs only while the game
  wants text: the console, the chat line or a menu text field
  (`WindowHost::SetTextWanted`, `WantsText` in `mp_client_input.cpp`).
- A tap clicked where the previous tap was: the engine took the pointer's
  position once a frame but the click at once. A click now hands over a
  moved pointer first (`ClientWindow::DeliverPointer`).
- Models (the car, the hands and gun) were flat colours. Their texture
  coordinates and normals are UBYTE4 vertex data, which the adapter reads as
  `R8G8B8A8_USCALED`; Adreno reads no USCALED vertex data. Such a device now
  gets them as `R8G8B8A8_UNORM`, and the vertex shaders reading them scale
  them back to bytes (`Device::byteStandIn`, `byteInputs` in
  `legacy_shader::BuildSpirv`). `KISAK_VK_BYTE_INPUTS=unorm` takes that path
  on any device; CI runs the Legacy checks with it, and the `byte_inputs`
  scene fails without the scale.

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
