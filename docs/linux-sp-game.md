# Native Linux singleplayer game (KisakCOD-sp)

`KisakCOD-sp` is the singleplayer game in one native Linux process (i386, or
x86_64 on the [64-bit track](linux-x64.md#x86_64-singleplayer-game)): server
(in its own thread, as on Windows), AI, game, cgame, client, UI, sound and
renderer, from the Windows `KisakCOD-sp` source lists. It is opt-in and off by
default, and built the way the multiplayer client is
([linux-mp-client.md](linux-mp-client.md)).

Status: killhouse and cargoship load and play on a developer laptop (Intel
GPU, DXVK, cinematics on): the pregame ends by itself, about a minute of play,
clean quit. Without game files it stops where the engine needs them (the
autoconfigure step's `configure.csv`), which is how CI runs it. Those runs
were the i386 game. The x86_64 game loads all 21 levels in the sweep tool on
the same laptop (2026-11-10, cinematics on, nobody watching the screen); by
hand it has not been played yet.

## Building

```sh
cmake -S . -B build-linux/sp -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON \
  -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<dxvk prefix>/include/dxvk \
  -DKISAK_DXVK_NATIVE_LIBRARY=<dxvk prefix>/lib/libdxvk_d3d9.so \
  -DKISAK_OPENAL_LIBRARY=/usr/lib/i386-linux-gnu/libopenal.so.1 \
  -DSDL3_DIR=<sdl3 prefix>/lib/cmake/SDL3 \
  -DKISAK_BUILD_LINUX_SP_GAME=ON
cmake --build build-linux/sp --target KisakCOD-sp
```

The libraries are the multiplayer client's: the native D3D9 boundary with a
DXVK Native library, SDL3 3.2.10 or newer and an OpenAL library, all built for
the game's architecture (i386 above). CI links
the same stand-ins (no D3D9 device, no OpenAL device). Add
`-DKISAK_LINUX_CINEMATICS=ON` to play the Bink movies through the FFmpeg shim
([linux-cinematics.md](linux-cinematics.md)); without it no movie plays. Both
games can be switched on in one build directory.

The Vulkan back end is linked in the way the multiplayer client links it: add
`-DKISAK_BUILD_VULKAN_PROBE=ON -DKISAK_BUILD_VULKAN_SHADERS=ON
-DKISAK_BUILD_VULKAN_LEGACY=ON -DKISAK_BUILD_VULKAN_BACKEND=ON` (Vulkan
headers needed; the loader is opened at run time through SDL, never linked),
and `+set r_backend vulkan` then draws through it instead of DXVK
([vulkan-d3d9-front.md](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan)).
The default stays `r_backend d3d9`. This is build proof only so far: the game
compiles and links with the back end and starts as before without game files,
but no singleplayer level has been drawn through Vulkan yet.

For x86_64, add `-DKISAK_LINUX_ARCH=x86_64` in a build directory of its own
and point the three libraries at x86_64 builds, as for the x86_64 multiplayer
client ([linux-x64.md](linux-x64.md#x86_64-multiplayer-client)). The aarch64,
armv7 and macOS configurations refuse the option.

## What it compiles

- The Windows `KisakCOD-sp` groups that the singleplayer compile census covers
  (`run_client_compile_census.py --target sp`), with `KISAK_SP`, minus what that
  census blocks (`scripts/tests/sp_compile_census_blocked.txt`). Every blocked
  unit is one the multiplayer client replaces too, and it is replaced the same
  way here: the renderer units on the native D3D9 proof list compile through the
  boundary, `r_cinematic.cpp` is built without `CINEMA` (or on the Bink shim),
  and the Windows platform units give way to `src/linux`.
- The native platform layer (`src/linux`) as `KisakCOD-mp` links it. Singleplayer
  adds its own thread contexts and the server thread's events to
  `src/linux/sys_threads.cpp` (the `KISAK_SP` block of `src/qcommon/threads.cpp`,
  same reset modes and initial states), and skips the multiplayer-only
  `net_restart`/`net_listen` commands and the start-up stats read, as
  `win_main.cpp` does.
- The game window, input, `msvc_rand.cpp`, `texture_memory.cpp` and the
  Material_Sort span, as `KisakCOD-mp`.
- `src/linux/client/mp_client_unavailable.cpp` with `KISAK_SP`: no remote script
  debugger, raw-asset material loading refused by name. Its multiplayer-only
  entries (HTTP downloads, voice) drop out.

## Checks

`scripts/tests/test_linux_sp_game.py` checks the module and, with
`--binary`, starts the game from temporary trees with no game data on SDL's dummy
drivers: an empty base path is refused at the file system check, and a one-line
stub of `fileSysCheck.cfg` gets through the file system to autoconfigure and
stops there. CI builds and runs it in the device-probe job (i386, Debug and
RelWithDebInfo) and in the x86_64 client job (Debug), where the 32-bit layout
checks also read the singleplayer sources. The Vulkan engine job builds the
i386 game with the Vulkan back end linked in and runs the same check on it:
without game files the game stops before the renderer, so this shows that the
back end builds, links and leaves the start alone, not that it draws.

## Fixes the first level loads found

- Fast skinning (`r_fastSkin`, which autoconfigure turns on) took the
  "no history" marker for last frame's normals offset and read far outside
  the buffer (`R_SkinXModelCmd`). Singleplayer keeps no skin-cache history,
  so every animated model crashed the worker threads.
- `CIN_PlayCinematic` is the multiplayer body. A level's pregame cinematic
  closed the pregame menu and moved the client to the cinematic state while
  rendering was held, so the level never started (an assert on Linux, a
  black screen in a Windows Release build). In singleplayer it now plays
  under the pregame menu. This is the port's reading of the pregame flow,
  not decompiled code.
- The Linux CPU benchmark is now the Windows one, so autoconfigure picks
  its CPU row on the same scale.
- A game built without movies (`KISAK_LINUX_CINEMATICS` off) loaded no level:
  the loading screen samples the movie's images, the build's stand-in never
  set them, and the frame stopped with "Tried to use 'cinematicY' when it
  isn't valid" behind a render thread assert. The stand-in now sets them to
  the black picture the full build shows while no movie has a frame. Found
  by the first x86_64 sweep on the laptop; not re-run on a level yet (the
  sweep that followed had movies on).
- Four structs were read through a void pointer in slots that fit the 32-bit
  layout only. One of them crashed the 64-bit game 4.5 s into airlift (an
  effect model's physics preset read as null; seen on Windows x64). They are
  read by their fields now, and `check_32bit_layout_offsets.py` reports the
  shape. With the fix the x86_64 Linux game holds airlift for 45 s and quits
  cleanly (laptop sweep); the Windows x64 game has not been re-run yet.

## Next

1. Longer play and more levels, with sound: what the other levels use, the
   sweep tool (`scripts/tests/run_linux_sp_level_sweep.py`) and the run plan
   are in [linux-sp-levels.md](linux-sp-levels.md).
2. The x86_64 game by hand, and its level chain and save and load in the
   sweep tool.
3. A first level through `r_backend vulkan` on the laptop, to find what the
   singleplayer levels ask of the D3D9 front that multiplayer maps do not.
4. Compare with the Windows `KisakCOD-sp` build on the same data, to tell
   port bugs from bugs the Windows build shares. Linux keeps assertions on in
   every configuration and the Windows Release build does not, so compare
   assertions with a Windows Debug build.
