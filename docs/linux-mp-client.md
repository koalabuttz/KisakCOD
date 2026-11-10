# Native Linux multiplayer client (KisakCOD-mp)

`KisakCOD-mp` is the whole multiplayer game in one native Linux process (i386,
or x86_64 as described in [linux-x64.md](linux-x64.md#x86_64-multiplayer-client);
an aarch64 build compiles and passes the asset-free checks, see
[linux-arm64.md](linux-arm64.md#building-the-aarch64-client)):
server, game, cgame, client, UI, sound and renderer, as the Windows `KisakCOD-mp`
target builds them. It is opt-in and off by default. It reached the
player-spawn milestone: a listen server whose own client joins it over the
engine's in-process loopback (see [linux-client.md](linux-client.md#joining-servers)).

Status: it links and starts, and its game window takes keyboard and mouse
input. Without game files it stops where the engine needs them. On a developer
laptop with the game files and a GPU (not CI evidence), `+devmap mp_crash`
reaches the in-game state, the team and class menus spawn the player in first
person, and the player can move, shoot and throw grenades, with sound, and quit
cleanly. The x86_64 build does the same. See [On the laptop](#on-the-laptop).
Movies are opt-in ([linux-cinematics.md](linux-cinematics.md)).

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
whose `alcOpenDevice` finds no device. For an x86_64 build, add
`-DKISAK_LINUX_ARCH=x86_64` and use x86_64 builds of the same libraries
([linux-x64.md](linux-x64.md#x86_64-multiplayer-client)).

The client draws through DXVK Native by default. `-DKISAK_BUILD_VULKAN_BACKEND=ON`
(with the Vulkan options it needs) also links this project's own Vulkan back
end, chosen at start with `+set r_backend vulkan`; DXVK stays the default
([vulkan-d3d9-front.md](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan)).

For profiling, add `-DKISAK_LINUX_TRACY=ON`. It builds `KisakCOD-mp`,
`KisakCOD-sp` and `KisakCOD-dedi` with the Tracy profiler (the version
`scripts/extern/tracy.cmake` fetches): the engine's `PROF_SCOPED` zones
become Tracy zones, one `MainThread` zone per frame. It stays idle until a
profiler of the same Tracy version connects from the same machine, for example
`tracy-capture -o run.tracy -s 120`. Player builds leave it off. CI builds the
Debug client with it on, so it keeps compiling and linking.

The SDL3 3.2 SDK build used on the laptop links its own i386 `libpipewire`,
found next to `libSDL3.so`. The client's run path covers its own libraries,
not their dependencies, so keep the SDK's `lib` directory on `LD_LIBRARY_PATH`
when running the client. OpenAL may then log a PipeWire error while it opens
the device and still play sound through another backend.

At startup the client prints one line naming the OpenAL device and the rate it
mixes at. For sound problems, `+set snd_alDebug 1` adds OpenAL errors, stream
underruns and a summary every 5 seconds (sounds started, sounds cut while still
playing, sources playing, longest gap between sound updates); `snd_alDebug 2`
also prints every sound start. It is off by default and changes nothing that is
played.

The OpenAL headers (`AL/al.h`) are needed too, whichever library is linked,
because the sound headers include them. Without `libopenal-dev` installed, unpack
the package somewhere (`apt-get download libopenal-dev`, then `dpkg-deb -x`) and
pass `-DKISAK_OPENAL_INCLUDE_DIR=<that tree>/usr/include`; nothing needs to be
installed system-wide. Use the setting from a fresh build directory, not one
that was configured with `-isystem <that tree>/usr/include` in the compiler
flags. CMake caches that directory as one the compiler already searches, and
then drops it from every target, so `AL/al.h` goes missing.

## What it compiles

`scripts/platform/linux/mp_client.cmake` has the details. In short:

| Part | Source |
| --- | --- |
| Engine | The source groups of the Windows target that the client compile census covers, with the client definitions, minus what the census still blocks. |
| Renderer | The units on `native_d3d9_renderer_sources.txt` (24, now including `r_screenshot.cpp`) and `r_utils.cpp` go through the native D3D9 boundary. The rest keep the type-only stand-in. |
| Windows platform units | `mp_client_platform_sources.txt` lists the ones linked as they are: `win_storage.cpp`, the player's stats. The census compiles them too. |
| Cinematics | `r_cinematic.cpp` is built without `CINEMA` by default. That is the engine's own build with no Bink: no cinematic ever plays. `CINEMA` is read nowhere else. With `KISAK_LINUX_CINEMATICS=ON` it is built with `CINEMA` on the Linux Bink shim, which decodes with FFmpeg's libavcodec ([linux-cinematics.md](linux-cinematics.md)). |
| Platform | `src/linux` as the dedicated server uses it, without its server-only stand-ins and its non-interactive script debugger. The client links the in-game debugger, `scr_debugger.cpp`. |
| Client platform | `msvc_rand.cpp`, the SDL3 Win32 window layer, `texture_memory.cpp` (`r_texturemem.cpp`'s answer without DirectDraw) and the generated `Material_Sort` span of `r_material_load_obj.cpp`. |
| Window and input | `mp_client_window.cpp` and `mp_client_input.cpp`, below, over the window tests' `WindowHost` and `EngineInput`. |
| Not available yet | `src/linux/client/mp_client_unavailable.cpp`, below. |

The entry point is `src/linux/main.cpp`, built with `KISAK_LINUX_MP_CLIENT`. It
prints the client's banner and usage, and reports the video card as DXVK's,
which the renderer names once it opens the device. After `Com_Init` it queues
`readStats`, as the Windows `WinMain` does. Console output goes to stderr only,
because the client's `Com_PrintMessage` already writes every line there.

## The game window and input

On Windows the window procedure (`win_wndproc.cpp`) and `win_input.cpp` feed
the engine. Here `src/linux/client/mp_client_window.cpp` and
`src/linux/client/mp_client_input.cpp` do:

| Windows | Linux |
| --- | --- |
| `WM_CREATE` sets `g_wv.hWnd` | The Win32 layer reports each new window to an observer from inside `CreateWindowExA`. The client adopts the renderer's window there with a `WindowHost`, which pumps its events but never presents to it or destroys it, and sets `g_wv.hWnd`. |
| `WM_DESTROY` clears it | `DestroyWindow` reports first, so the host lets go before the window goes (a `vid_restart` or shutdown). Letting go neither quits nor touches the engine's keys. |
| `PeekMessage` in `Win_GetEvent`, which only sees the calling thread's windows | `Win_GetEvent` pumps the window once the engine queue is empty, on the thread that created it. The render thread also calls `Win_GetEvent` (`Sys_LoadingKeepAlive` while it draws the loading screen); there the pump does nothing, as `PeekMessage` finds nothing. |
| Keys, mouse buttons, wheel, `WM_CHAR` | `SE_KEY` and `SE_CHAR` in the engine queue, through `EngineInput`. Keys follow their US layout positions. |
| `IN_MouseMove`, once per frame | `IN_Frame` hands `CL_MouseEvent` the last position (in window pixels) and the motion summed since the last frame. |
| The cursor recentred while `CL_MouseEvent` answers yes | Relative capture while it answers yes (look mode). When a menu or the console has the mouse, capture is released and the cursor follows `IN_ShowSystemCursor`. |
| `WM_ACTIVATE` runs `VID_AppActivate` | Focus and minimize changes run the same steps, after the window releases the held keys. |
| `WM_CLOSE` runs `Com_Quit_f` | Queues `quit`. Quitting shuts the renderer down, and with it the window, so it waits for the next command buffer pass instead of running inside the window's own event pump. |

DXVK Native has to present through its SDL3 window system, because the
renderer's windows are SDL3 windows. The client sets `DXVK_WSI_DRIVER=SDL3`
before the engine starts, and refuses to start when the variable names another
one.

Fullscreen on a native Wayland session needs DXVK 3.0 or later, or 2.7.1 with
upstream commit `a1abc9c4` ("[wsi] Don't treat SDL_SetWindowPosition failure
as fatal") on top. DXVK 2.7.1's SDL3 path positions the window before going
fullscreen, Wayland refuses to position it, and 2.7.1 gives up. With that one
commit, the laptop played fullscreen on GNOME Wayland, and Alt-Tab worked. The
project still builds against 2.7.1's headers. Only the library the client loads
at run time needs the fix, so putting the patched library first on
`LD_LIBRARY_PATH` is enough. Without it, run windowed (`+set r_fullscreen 0`),
or use XWayland (`SDL_VIDEO_DRIVER=x11`). Fullscreen uses `r_mode` as on
Windows, so set it to the panel's size for a sharp picture; a smaller mode is
scaled up. `vid_restart` is refused while a listen server runs, as in the
retail game.

The engine's own `SIGINT`/`SIGTERM` handling stays in place: the client tells
SDL not to install its signal handlers. Pointer boundaries the adapter queues
(`SE_POINTER`) are dropped before `Com_EventLoop`, which has no case for them.
Joysticks, MIDI and DirectInput are not ported; the Windows build does not use
them either.

## Where it writes

Configs, logs and player profiles go to `fs_homepath`. The engine's default for
it is `fs_basepath`, the game folder, so the Linux client picks its own default
when the command line sets none: `$XDG_DATA_HOME/KisakCOD`, or
`~/.local/share/KisakCOD` when `XDG_DATA_HOME` is unset or not absolute. With
neither that nor `HOME` usable it refuses to start and asks for
`+set fs_homepath <folder>`. Files in the game folder are still read, through
the search path. The macOS build uses `~/Library/Application Support/KisakCOD`
instead ([macos.md](macos.md#multiplayer-client)).

This keeps the client away from the game's own stats. This build has no CD key
(`cl_cdkey` stays blank, on Windows too), and the stats file (`mpdata`) is
encrypted with a key derived from it. So a stats file the retail game wrote
cannot be decrypted. `LiveStorage_HandleCorruptStats` then renames and deletes
it under `fs_homepath`, writes zeroed stats, and drops the game with
`PLATFORM_STATSREADERROR`. With the default home folder, the retail file in the
game folder is only read. The first map load with an existing retail profile
drops once. After that, the client reads its own zeroed stats from the home
folder.

Retail never separated the two folders, so the engine only searched
`fs_basepath/players` and created or deleted profiles there, while every other
`players/` write went to `fs_homepath`. The engine now also searches
`fs_homepath/players`, ahead of the game folder's, and creates and deletes
profiles under `fs_homepath`. When the two paths are the same, as by default on
Windows, nothing changes. Deleting a profile that exists only in the game
folder does nothing.

## What it does not have yet

`mp_client_unavailable.cpp` answers for each missing service the way the
engine answers on Windows when that service is missing:

| Service | Windows owner | Answer on Linux |
| --- | --- | --- |
| Remote script debugger | `win_net_debug.cpp` | None attached. `net_listen` says it is unavailable. |
| HTTP downloads | `dl_main.cpp` (libcurl) | A download cannot begin, so the client sends `wwwdl fail` and falls back to the UDP download. |
| Voice chat | `win_voice.cpp` (DirectSound) | No voice device: nothing is captured, sent or played. |

Functions that only raw-asset loading (`useFastFile 0`) or an attached remote
debugger reaches abort with their name. `Material_FreeAll` runs its fast-file
branch: it releases the technique sets and nothing else.

The default build has no Steam ticket provider, so it joins a remote server
only with a key code, and only where the server accepts unverified GUIDs
(`sv_allowUnverifiedGuids 1`, [key-codes.md](key-codes.md)). With no key code
the join is refused before any packet is sent
([linux-client.md](linux-client.md#joining-servers)).
Build with `-DKISAK_LINUX_STEAM=ON` to authenticate through the running Steam
client instead ([linux-steam.md](linux-steam.md)).

## Checks

```sh
LD_LIBRARY_PATH=<sdl3-sdk>/lib:<d3d9 stand-in dir>:<openal stand-in dir> SDL_VIDEODRIVER=dummy \
  python3 scripts/tests/test_linux_mp_client.py --binary <bin>/KisakCOD-mp
```

`scripts/tests/test_linux_mp_client.py` checks the sources first: the option
is off by default (i386, or x86_64 under `KISAK_LINUX_ARCH=x86_64`), `CINEMA` is read only by `r_cinematic.cpp`, the
platform list names real `src/win32` files, and the stand-ins do not touch
authentication. With `--binary` it then runs the client from temporary trees
with no game data:

- `--help` and `--version` exit 0 before the engine starts.
- An empty base path is refused at the file system check.
- A tree with only a stub `fileSysCheck.cfg` gets through the file system and
  the player profile to the client's autoconfigure step. It stops there because
  `configure_mp.csv`, a game file, is missing.

- Without `fs_homepath`, the same tree writes only under
  `$XDG_DATA_HOME/KisakCOD`, or `~/.local/share/KisakCOD` when there is no
  absolute `XDG_DATA_HOME`. With neither that nor `HOME`, the client refuses
  before the engine starts.
- With an active profile in both folders, the one in `fs_homepath/players`
  is used, that folder comes first in the search path, and the game folder's
  `players/` is left as it was.

These runs exit 1, print each line once, try no connection and write only under
`fs_homepath`. The renderer never starts in these runs, so no window is made.

`KisakCOD-mp-client-window-test` (`tests/linux_mp_client/client_window_test.cpp`)
checks the game window on SDL's dummy video driver, with the engine replaced by
a recorder. That driver cannot make Vulkan windows, so the test drops the Vulkan
flag from the window `CreateWindowExA` asks for; the rest is the client's code.
It checks that the window is adopted inside `CreateWindowExA` and let go inside
`DestroyWindow`, keys and text reach the engine queue, the mouse is reported
once per frame, look mode takes and releases capture, focus, minimize and close
reach the engine, and a destroyed window neither quits nor clears keys. It does
not check a real desktop's events or HiDPI scaling.

```sh
LD_LIBRARY_PATH=<sdl3-sdk>/lib SDL_VIDEODRIVER=dummy <bin>/KisakCOD-mp-client-window-test
```

CI builds both in the device-probe job with the stand-ins and runs both.

## A folder that runs on another PC

A client built as above runs where it was built: its run path names that
machine's SDL3 and DXVK folders. `scripts/tools/package_linux_client.py` makes a
folder that can be copied to another Linux PC of the same architecture:

```sh
cmake ... -DKISAK_BUILD_LINUX_MP_CLIENT=ON -DKISAK_LINUX_PORTABLE_RUNPATH=ON
cmake --build <build> --target KisakCOD-mp
python3 scripts/tools/package_linux_client.py \
  --binary <bin>/KisakCOD-mp \
  --lib-dir <sdl3-sdk>/lib --lib-dir <dxvk>/lib \
  --license SDL3=<sdl3-source>/LICENSE.txt --license DXVK=<dxvk-source>/LICENSE \
  --label "$(git rev-parse --short HEAD)" --out KisakCOD-linux --archive
```

`KISAK_LINUX_PORTABLE_RUNPATH` (off by default) makes the client's run path
`$ORIGIN/lib` and nothing else. The tool refuses a binary whose run path names
build folders, because on another PC the loader would still look in them
first, and there they are folders anyone may be able to create
(`--allow-build-runpath` packs it anyway, for a folder that stays on the build
machine). With the option on, the build tree's own client needs
`LD_LIBRARY_PATH` to start, as the checks above already set it.

The tool reads each file's ELF `NEEDED` list and copies, by soname, every
library it finds in a `--lib-dir`, and what those need in turn (the SDL3 SDK's
own libraries next to `libSDL3.so`, for example). `--also <soname>` adds a
library that is only loaded by name at run time. It runs nothing and reads no
game file. What it does not find stays the other PC's job and is listed in the
folder's `README.txt`: typically OpenAL, and FFmpeg or libcurl when those
options are on. The C library, `libstdc++`, `libgcc_s` and the Vulkan and GL
libraries are never packed, whatever the folders hold.

The folder holds the binary, `lib/`, a launcher, `README.txt`, `MANIFEST.txt`
(size and SHA-256 of every file) and the licence texts given with `--license`.
The launcher puts `lib/` first on the library path, which also covers what the
packed libraries need themselves, and passes the game folder as `fs_basepath`:

```sh
KisakCOD-linux/run-mp.sh --game "<Call of Duty 4 folder>" +set r_fullscreen 0
```

The game folder can also come from `KISAKCOD_GAME_DIR` or be the current
folder. For fullscreen on a native Wayland session, pack a DXVK build that has
the fix named under [The game window and input](#the-game-window-and-input).
Several binaries of one architecture can share a folder (`--binary` again for
`KisakCOD-sp` or `KisakCOD-dedi`; each gets its own `run-*.sh`).

`scripts/tests/test_package_linux_client.py` checks the tool with a made-up
binary and SDK built by the host compiler: the packed folder still runs after
the SDK is moved away. Hosted CI also packs the x86_64 client with the SDL3 SDK
and the stand-ins and starts it through the launcher with no library path set.
No packed folder has been tried on a second real PC yet.

## On the laptop

These steps need a GPU and the game files, so they ran on a developer laptop
(i386 build, DXVK Native over SDL3, system OpenAL Soft) and are not CI
evidence. All three pass as of master `998344d`:

1. `+set fs_basepath <game folder> +devmap mp_crash` reaches the in-game state
   (`clientinfo` shows state 9), with sound.
2. Picking a team and class spawns the player, in first person. From the
   console: `mr <serverid> 8 autoassign`, then `mr <serverid> 15 assault_mp,0`.
   Read `<serverid>` with `sv_serverid`. The second number is the menu's index
   among the script menu configstrings; on mp_crash 8 is the team menu and 15
   the class menu. The menus in the window work as well.
3. Moving works from the console (`+forward`, `+moveright`, `+right`), and by
   hand: the player runs, aims with the mouse, shoots and throws grenades.
   With `cg_showmiss 1`, scripted moves printed no prediction errors. Four
   minutes of play printed about 140 prediction misses, 1.2 units on average
   and 7.7 at most, and about 190 prediction errors. The Windows build over
   similar play (2026-11-04, a desktop PC) printed 32 misses, 0.47 units on
   average, and 34 errors. Later laptop runs put the difference down to what
   happened in play (concussion grenades, obstacles), not to the Linux build:
   by hand both builds miss about as often per minute
   ([status](status.md#prediction)).

The x86_64 build passed the same steps on the same laptop on 2026-11-04,
including a clean quit ([linux-x64.md](linux-x64.md#x86_64-multiplayer-client)).

Run it with `+set net_ip 127.0.0.1`, and windowed
(`+set r_fullscreen 0 +set r_mode 1024x768`) unless the DXVK library has the
fullscreen fix above. The listen server then opens no outside port. The
first map load with an existing retail profile drops once (see
[Where it writes](#where-it-writes)).

### Map sweep

Only mp_crash has been played by hand. `scripts/tests/run_linux_mp_map_sweep.py`
runs `KisakCOD-mp` against an install on the other maps and writes a report. It
is not run by CI; `scripts/tests/test_linux_mp_map_sweep.py` checks the tool
itself against a stand-in game.

```sh
python3 scripts/tests/run_linux_mp_map_sweep.py "$GAME" --binary "$BUILD/KisakCOD-mp" \
    --work-dir ~/kisak-private/mp-sweep1 --screenshot --sound-debug
```

For each of the 21 stock maps (or `--maps a,b`) it starts a listen server with
`+devmap`, bound to 127.0.0.1, and then:

1. waits for `Server: <map>` and for `clientinfo` to answer `state: 9`. A first
   load that drops back to the menu is asked for once more and counted;
2. reads `sv_serverId` and the script menus from the `configstrings` listing,
   and sends the two menu responses of step 2 above. The menus are taken by
   position on the first map (`--team-menu 8 --class-menu 15`) and by the names
   found there on the later ones;
3. presses buttons through the console for about 25 seconds: fire, grenade,
   run, sprint, jump, reload, next weapon, crouch, strafe (`--no-exercise`
   skips this);
4. holds until `--hold` seconds have passed since the join (default 45), then
   checks that `mapname` answers with the map and the client is still in
   state 9;
5. with `--screenshot`, takes one screenshot with the game's own command and
   fails the step when the file is missing or nearly all one colour. The
   report describes each image in numbers (mean brightness, the share that is
   not black, how many colours, the share of the commonest colour, the mean
   colour of the top, middle and bottom third), so a map that stands apart
   from the others shows without the images leaving the machine.
   `--delete-screenshots` removes each image once it is measured; otherwise
   they stay in the work directory for someone to look at;
6. sends `kill`, which only acts on a playing client, and looks for the death
   the stock scripts log (a `K;` line in `games_mp.log`). That shows the join
   really spawned the player;
7. quits and records the exit status.

`--plan chain` runs every map in one process, changing with `devmap` as a map
rotation does. `--hold 1800 --maps mp_crash` is a long soak of one map.
`--sound-debug` sets `snd_alDebug 1` and adds the OpenAL driver's counts per
map (sounds started, cut while playing, stream underruns, OpenAL errors, the
longest gap between sound updates). `--assert-continue`, `--gdb` and
`--set DVAR VALUE` work as in the
[singleplayer sweep](linux-sp-levels.md#the-sweep-tool).

The report counts assertions, errors, script errors and warnings per map and
names the step a map stopped at. It does not say a map looks or sounds right.
Everything the game writes goes under the work directory (`base/` with symlinks
to the install's `main`, `zone` and `localization.txt`, `home/` as
`fs_homepath` with a profile named `sweep`), and the install's `players/`
folder is not staged. The logs there hold the console output, asset names
included, so keep the directory private.

No laptop run of the sweep has been made yet.

### Flashbang and concussion screen effects

`scripts/tools/shellshock_screenshot_check.py` checks the flashbang white-out
and the concussion effect from the game's own screenshots, with nobody
playing. `write-cfg <home>/main/shk.cfg` writes a config whose `vstr shk_go`
waits about 15 s, takes seven screenshots from the still spectator view and
quits. Each shock (`cg_shellshock 4 flashbang`,
`cg_shellshock 4 concussion_grenade_mp`) is shot twice, with
`cg_drawShellshock` 1 and 0, so the camera shake is the same and only the
screen effect differs. `check <home>/main/screenshots` prints brightness and
edge-strength numbers and PASS or FAIL.

- Windows: add `+exec shk.cfg +devmap mp_crash +wait 5000 +vstr shk_go` to
  the usual command line and run it from the game folder.
- Linux: `+wait` lets the map connect before the start-up stats load and
  trips the stats upload assert. Launch with `+exec shk.cfg +devmap mp_crash`
  and type `vstr shk_go` on stdin once the stats have arrived. Add
  `+set vid_xpos 0 +set vid_ypos 0` when the window fills the display, or
  `screenshot` refuses a window it sees as partly off screen.
- `screenshot` reads the front buffer, so the window must be uncovered.

Results on 2026-11-08 (local runs, not CI evidence): the flashbang whites
out and the concussion effect draws on Windows Win32 and x64 (CI build of
47017e3, RTX 2080 Super) and on the x86_64 Linux client (laptop, Intel
graphics through DXVK). The concussion effect blends the previous frame back
in at about 99% weight; from a still view the frame drifts brighter on the
NVIDIA PC and darker on the laptop rather than smearing, which looks like
8-bit blend rounding in the feedback loop. On the i386 Linux client the
screenshots were unreliable (several frames taken seconds apart came out
identical, one an older view), so i386 rests on the earlier checks by hand.
