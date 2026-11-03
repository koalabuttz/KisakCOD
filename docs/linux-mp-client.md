# Native Linux multiplayer client (KisakCOD-mp)

`KisakCOD-mp` is the whole multiplayer game in one native i386 Linux process:
server, game, cgame, client, UI, sound and renderer, as the Windows `KisakCOD-mp`
target builds them. It is opt-in and off by default. It is the target for the
player-spawn milestone: a listen server whose own client joins it over the
engine's in-process loopback (see [linux-client.md](linux-client.md#joining-servers-local-yes-remote-no)).

Status: it links and starts. Without game files it stops where the engine
needs them. Nothing has run on a GPU or with game files yet; that is the next
step, on a developer machine.

## Building

```sh
cmake -S . -B build-linux/mp -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON \
  -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<dxvk prefix>/include/dxvk \
  -DKISAK_DXVK_NATIVE_LIBRARY=<dxvk prefix>/lib/libdxvk_d3d9.so \
  -DKISAK_OPENAL_LIBRARY=/usr/lib/i386-linux-gnu/libopenal.so.1 \
  -DSDL3_DIR=<sdl3 prefix>/lib/cmake/SDL3 \
  -DKISAK_BUILD_LINUX_MP_CLIENT=ON
cmake --build build-linux/mp --target KisakCOD-mp
```

It needs the native D3D9 boundary ([linux-native-d3d9.md](linux-native-d3d9.md))
with a DXVK Native library, SDL3 3.2.10 or newer, and an i386 OpenAL library.
CI links stand-ins for the last two parts instead: the device probe's
`libd3d9_unavailable.so`, and `tests/linux_mp_client/openal_unavailable.cpp`,
whose `alcOpenDevice` finds no device. The 64-bit configuration refuses the
option.

## What it compiles

`scripts/platform/linux/mp_client.cmake` has the details. In short:

| Part | Source |
| --- | --- |
| Engine | The source groups of the Windows target that the client compile census covers, with the client definitions, minus what the census still blocks. |
| Renderer | The units on `native_d3d9_renderer_sources.txt` (24, now including `r_screenshot.cpp`) and `r_utils.cpp` go through the native D3D9 boundary. The rest keep the type-only stand-in. |
| Windows platform units | `mp_client_platform_sources.txt` lists the ones linked as they are: `win_storage.cpp`, the player's stats. The census compiles them too. |
| Cinematics | `r_cinematic.cpp` is built without `CINEMA`. That is the engine's own build with no Bink: no cinematic ever plays. `CINEMA` is read nowhere else. |
| Platform | `src/linux` as the dedicated server uses it, without its server-only stand-ins and its non-interactive script debugger. The client links the in-game debugger, `scr_debugger.cpp`. |
| Client platform | `msvc_rand.cpp`, the SDL3 Win32 window layer, `texture_memory.cpp` (`r_texturemem.cpp`'s answer without DirectDraw) and the generated `Material_Sort` span of `r_material_load_obj.cpp`. |
| Not available yet | `src/linux/client/mp_client_unavailable.cpp`, below. |

The entry point is `src/linux/main.cpp`, built with `KISAK_LINUX_MP_CLIENT`. It
prints the client's banner and usage, and reports the video card as DXVK's,
which the renderer names once it opens the device. After `Com_Init` it queues
`readStats`, as the Windows `WinMain` does. Console output goes to stderr only,
because the client's `Com_PrintMessage` already writes every line there.

## What it does not have yet

`mp_client_unavailable.cpp` answers for each missing service the way the
engine answers on Windows when that service is missing:

| Service | Windows owner | Answer on Linux |
| --- | --- | --- |
| Remote script debugger | `win_net_debug.cpp` | None attached. `net_listen` says it is unavailable. |
| HTTP downloads | `dl_main.cpp` (libcurl) | A download cannot begin, so the client sends `wwwdl fail` and falls back to the UDP download. |
| Voice chat | `win_voice.cpp` (DirectSound) | No voice device: nothing is captured, sent or played. |
| Mouse and keyboard | `win_input.cpp` | No mouse initialized. The console on stdin is the only input. |
| Window procedure | `win_wndproc.cpp` | `g_wv.hWnd` stays null, so the game window is never the foreground window. |

Functions that only raw-asset loading (`useFastFile 0`) or an attached remote
debugger reaches abort with their name. `Material_FreeAll` runs its fast-file
branch: it releases the technique sets and nothing else.

Joining a remote server is refused, because Linux has no Steam ticket provider
([linux-client.md](linux-client.md#joining-servers-local-yes-remote-no)).

Window events and input are the next piece. Until then the SDL window is not
pumped, so a desktop may report it as not responding during a run.

## Checks

```sh
LD_LIBRARY_PATH=<sdl3-sdk>/lib:<d3d9 stand-in dir>:<openal stand-in dir> SDL_VIDEODRIVER=dummy \
  python3 scripts/tests/test_linux_mp_client.py --binary <bin>/KisakCOD-mp
```

`scripts/tests/test_linux_mp_client.py` checks the sources first: the option
is off by default and i386-only, `CINEMA` is read only by `r_cinematic.cpp`, the
platform list names real `src/win32` files, and the stand-ins do not touch
authentication. With `--binary` it then runs the client from temporary trees
with no game data:

- `--help` and `--version` exit 0 before the engine starts.
- An empty base path is refused at the file system check.
- A tree with only a stub `fileSysCheck.cfg` gets through the file system and
  the player profile to the client's autoconfigure step. It stops there because
  `configure_mp.csv`, a game file, is missing.

Both runs exit 1, print each line once, try no connection and write only under
`fs_homepath`. CI builds the client in the device-probe job with both stand-ins
and runs this script.

## On the laptop

These steps need a GPU and the game files, so they run on a developer machine
and are not CI evidence:

1. `+set fs_basepath <game folder> +devmap mp_crash` reaches the in-game state
   (`CA_ACTIVE`).
2. Picking a team and class (`mr` menu responses from the console) spawns the
   player, and the frame is first-person.
3. Input and prediction work.
