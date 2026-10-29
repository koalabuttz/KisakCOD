# KisakCOD

## About the project
An open source fully-buildable reimplementation of Call of Duty 4

Aimed towards mod developers and COD4 enthusiasts.

![licimg](./GPLv3_Logo.png)

### Development Blog
Learn about the Development of KisakCOD here: [https://lwss.github.io/Duty-Of-Kisak/](https://lwss.github.io/Duty-Of-Kisak/)

## Current Requirements
- Windows OS
- Visual Studio 2022
- CMake >= 3.16
- [DirectX SDK 2010](https://www.microsoft.com/en-us/download/details.aspx?id=6812)
- Steam with a copy of [Call of Duty 4](https://store.steampowered.com/app/7940/Call_of_Duty_4_Modern_Warfare_2007/)


## How to build
1) Install the above requirements and Clone repo
2) Open a terminal and run `generate-project.bat`
3) Open .sln projects that are generated in `build-sp`, `build-mp`, and `build-dedi` respectively. 
4) Copy COD4 Game files to `bin/(BUILD_TYPE)/*` (Don't try to cherry-pick them, small files like localization.txt are needed)
5) Copy `deps/binklib/binkw32.dll` as well ^^
6) Copy all files in `deps/msslib/dlls/*` ^^ 
7) Copy `deps/steamsdk/steam_api.dll`  ^^
8) Run the game via Visual Studio play button or just the .exe


```
Keep in Mind: This is a ~20 year old game with some known exploits. We will try to fix these as we become aware of them.
However, there is a non-zero chance of some type of binary exploitation when playing online. Use a sandbox (Sandboxie?) for peace of mind. 
```

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

Radiant requires MSVC/MFC and is excluded from the MinGW configuration. The
MinGW port also retains D3DX shader helper stubs returning `E_NOTIMPL`, so a
successful build does not establish full rendering or gameplay compatibility.

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
