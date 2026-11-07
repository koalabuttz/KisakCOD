# KisakCOD

## About the project
An open source fully-buildable reimplementation of Call of Duty 4

Aimed towards mod developers and COD4 enthusiasts.

![licimg](./GPLv3_Logo.png)

### Development Blog
Learn about the Development of KisakCOD here: [https://lwss.github.io/Duty-Of-Kisak/](https://lwss.github.io/Duty-Of-Kisak/)

## Current Requirements

Windows game and tools:
- Windows OS
- Visual Studio 2022
- CMake >= 3.18
- [DirectX SDK 2010](https://www.microsoft.com/en-us/download/details.aspx?id=6812)
- Steam with a copy of [Call of Duty 4](https://store.steampowered.com/app/7940/Call_of_Duty_4_Modern_Warfare_2007/)

Native Linux (no Wine): CMake, Ninja, Clang and the 32-bit C++ development
libraries for the dedicated server; DXVK Native, SDL3 and OpenAL as well for the
opt-in game client (see [Linux](#linux-native) below). Licensed game assets are
only needed to run the game or a server, not to build or test them.

## How to build

### Windows (Visual Studio)

1) Install the above requirements and Clone repo
2) Open a terminal and run `generate-project.bat`
3) Open .sln projects that are generated in `build-sp`, `build-mp`, and `build-dedi` respectively. 
4) Copy COD4 Game files to `bin/(BUILD_TYPE)/*` (Don't try to cherry-pick them, small files like localization.txt are needed)
5) Copy `deps/binklib/binkw32.dll` as well ^^
6) Copy all files in `deps/msslib/dlls/*` ^^ 
7) Copy `deps/steamsdk/steam_api.dll`  ^^
8) Run the game via Visual Studio play button or just the .exe

For profiling, configure with `-DKISAK_WIN_TRACY=ON` (Win32, x64 or ARM64). It
builds `KisakCOD-sp`, `KisakCOD-mp` and `KisakCOD-dedi` with the Tracy profiler
(the version `scripts/extern/tracy.cmake` fetches): the engine's `PROF_SCOPED`
zones become Tracy zones, one `MainThread` zone per frame. The game stays idle
until a Tracy profiler or `tracy-capture` of the same version connects from the
same machine (it listens on localhost only). Player builds leave it off. CI
builds the Win32 Debug games with it on, so it keeps compiling; a manual run of
the Windows workflow with the `tracy` input builds Win32 Release and x64
profiling executables (artifacts named `-Tracy`). Linux has `KISAK_LINUX_TRACY`
([Linux client](docs/linux-mp-client.md)).


```
Keep in Mind: This is a ~20 year old game with some known exploits. We will try to fix these as we become aware of them.
However, there is a non-zero chance of some type of binary exploitation when playing online. Use a sandbox (Sandboxie?) for peace of mind. 
```

### Linux (native)

The native Linux port builds native ELF programs, no Wine. The default build is
the **32-bit x86 multiplayer dedicated server**. The game client is opt-in and
has its own guide. Where things stand: [Linux port status](docs/status.md).

1) Install the build tools (Debian/Ubuntu shown):

   ```sh
   sudo apt-get install cmake ninja-build clang g++-multilib
   ```

2) Build. The optional arguments are a build directory (default `build-linux`)
   and a configuration (`Debug`, `Release`, `RelWithDebInfo` (default) or
   `MinSizeRel`). Use one build directory per configuration.

   ```sh
   bash build-linux.sh
   # or, for example:
   bash build-linux.sh build-linux-debug Debug
   ```

   The executable is `bin/KisakCOD-dedi`.

3) Run the asset-free checks:

   ```sh
   python3 scripts/tests/test_linux_bootstrap.py
   python3 scripts/tests/run_linux_checks.py --build-dir build-linux --binary bin/KisakCOD-dedi
   ```

4) To run a server, point it at your own copy of the CoD4 game files. Licensed
   game assets remain external to the repository.

On a developer machine with the game files, the dedicated server loads all 21
stock maps and hosts bots through map cycles, and a 64-bit (x86_64) build of it
does the same ([x86_64 track](docs/linux-x64.md)).

**Game client.** `KisakCOD-mp` (`-DKISAK_BUILD_LINUX_MP_CLIENT=ON`) is the whole
multiplayer game in one native process, for i386 or x86_64. It renders through
the engine's own D3D9 renderer on DXVK Native, with SDL3 for the window and
OpenAL for sound. On a developer laptop with the game files it hosts a listen
server, spawns the player, and plays: moving, shooting, grenades, sound and a
clean quit. Movies play with the opt-in FFmpeg shim
([cinematics](docs/linux-cinematics.md)). Build steps:
[multiplayer client](docs/linux-mp-client.md). The singleplayer game
([guide](docs/linux-sp-game.md), i386) loads and plays its first levels,
killhouse and cargoship.

**Authentication.** Linux Steam authentication is unavailable, so the Linux
client cannot join a remote server, and the dedicated server rejects ordinary
client tickets. Two explicit opt-ins can admit a client without a ticket: a
loopback-only test mode, and `sv_allowUnverifiedGuids`, which accepts the retail
GUID handshake from any address without verification. With the latter on, GUIDs
are spoofable and must not carry bans, privileges or persistent identity. Stock
retail CoD4 1.7 and CoD4x 21.1 have each played in separate brief local sessions
against the Linux server. These are not simultaneous human-player, mixed-client,
public-server authentication or general compatibility proof.

Further reading:
- [Linux port status](docs/status.md): what works, what is verified and where,
  and what is missing
- [Linux dedicated server](docs/linux-dedicated.md): supported scope,
  prerequisites, tests and remaining work
- [All Linux docs](docs/README.md), including the [history](docs/history/README.md)
  of earlier progress notes

### Cross-build from Linux with LLVM-MinGW

The MinGW configuration produces a 32-bit Windows multiplayer executable. It
requires CMake, Ninja, and LLVM-MinGW's `i686-w64-mingw32-*` tools on `PATH`.
LLVM-MinGW 20260616 (LLVM 22.1.8, MSVCRT), CMake 3.31.10, and Ninja 1.13.0 are
the validated tool versions.

```bash
bash build-linux-for-windows.sh
# Optional: put build files outside the checkout.
bash build-linux-for-windows.sh /path/to/build-directory
# Optional: use an optimized build with debug symbols for runtime testing.
bash build-linux-for-windows.sh /path/to/optimized-build-directory RelWithDebInfo
```

The script builds `KisakCOD-mp` with `KISAK_OPENAL=ON`. The optional second
argument selects `Debug` (default), `Release`, `RelWithDebInfo`, or `MinSizeRel`.
Debug leaves optimization off; use `RelWithDebInfo` or `Release` for runtime
performance tests, especially under CPU emulation. Use separate build directories
to preserve each configuration's object files. The script reuses the selected
build directory and stops on configuration or compilation errors. CMake fetches
the pinned OpenAL Soft and Tracy versions during configuration. The executable
is written to `bin/KisakCOD-mp.exe`; the game assets and runtime DLLs described
above are still required to run it under 32-bit-capable Wine.

The MinGW build requires the x86 D3DX9_43 runtime from the official DirectX
End-User Runtimes (June 2010). Wine provides its own implementation. The original
game's `d3dx9_34.dll` alone does not satisfy this dependency. Shader compilation
and reflection use the real D3DX APIs; an asset-free
[shader runtime check](scripts/tests/d3dx_shader_runtime.md) verifies them.

Radiant requires MSVC/MFC and is excluded from the MinGW configuration. A
successful build does not establish full rendering or gameplay compatibility.

### Renderer probes

Before the game client could draw, the renderer was brought up in separate
opt-in probes: a [standalone surface replay](tests/linux_renderer/README.md)
checked against CPU pixel predictions, then the engine renderer drawing a map
([world draw probe](docs/linux-world-draw-probe.md)) and a
[free camera](docs/linux-free-camera.md) flying through it. Their asset-free
parts still run in CI without a GPU, DXVK, SDL or game assets.

## Known Issues
(Use the **[issues](https://github.com/SwagSoftware/KisakCOD/issues)** section)

## Troubleshooting
- ***Can't Connect to Dedicated Server*** :
  -  Check `net_ip` and `net_port`, the server will increment the port if the preferred one isn't available but the client won't sweep upwards.
 - ***DLL Error upon launch*** :
   - You didn't copy over the necessary runtime DLL's

## FAQ
- Can we use AI in this project?
  - Yes you can, but you're still responsible for whatever you commit. In general, you should have the AI be assisting you, and not carrying you. We have started using AI to help de-bug, and it's been extremely helpful.

## Credits and Special Thanks
- ***All Original COD4 Developers (for creating one of the best games of all time)***
- https://github.com/PJayB/jk3src (Jedi Academy fork with .sln)
- https://github.com/voron00/CoD2rev_Server - Useful yacc code for the gsc scripting here
- https://github.com/shiversoftdev/BO3Enhanced - Viewed as reference code for some of the Steam API Auth
- [RAD Game Tools](https://www.radgametools.com/) for their Bink and Miles Sound System libraries.
- [ODE Physics](https://www.ode.org/) COD4 uses a modified version of this physics engine.


## Discord
[Join the KisakCOD Discord](https://discord.gg/9uqntRWMA3)
