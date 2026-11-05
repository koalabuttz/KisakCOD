# Native Linux singleplayer game (KisakCOD-sp)

`KisakCOD-sp` is the singleplayer game in one native i386 Linux process: server
(in its own thread, as on Windows), AI, game, cgame, client, UI, sound and
renderer, from the Windows `KisakCOD-sp` source lists. It is opt-in and off by
default, and built the way the multiplayer client is
([linux-mp-client.md](linux-mp-client.md)).

Status: it links and starts. Without game files it stops where the engine needs
them (the autoconfigure step's `configure.csv`). Nothing has loaded a level yet:
that needs the game files and a GPU, so it is run on a developer machine.

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
DXVK Native library, SDL3 3.2.10 or newer and an i386 OpenAL library. CI links
the same stand-ins (no D3D9 device, no OpenAL device). Add
`-DKISAK_LINUX_CINEMATICS=ON` to play the Bink movies through the FFmpeg shim
([linux-cinematics.md](linux-cinematics.md)); without it no movie plays. The
64-bit configuration refuses the option for now. Both games can be switched on
in one build directory.

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
stops there. CI builds and runs it in the device-probe job, Debug and
RelWithDebInfo.

## Next

1. A first level load on the laptop with the game files (`+devmap killhouse`),
   with and without the movie shim.
2. A baseline of the Windows `KisakCOD-sp` build on the same data, to tell
   port bugs from bugs the Windows build shares.
