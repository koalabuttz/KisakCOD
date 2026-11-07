# PS Vita dedicated server

`KISAK_PLATFORM=vita` builds the multiplayer dedicated server, `KisakCOD-dedi`,
for a hacked PS Vita (HENkaku/Ensō), packed as `KisakCOD-dedi.vpk`. It is an
opt-in track started 2026-11-07, after the [armv7 Linux build](linux-armv7.md)
proved the engine on the Vita's CPU family. Nothing here changes the Linux,
macOS or Windows builds.

**State:** the server compiles, links and packs into a `.vpk` (sandbox and
hosted CI). It has **not run on a Vita yet**: start-up, the data folder, the
network and map loading are first checked on David's console with the steps
below.

## Building

Needs Clang (the engine's Microsoft C++ extensions rule out GCC), CMake, Ninja
and VitaSDK. `scripts/ci/install-vitasdk.sh` downloads the pinned VitaSDK
snapshot from the vitasdk/autobuilds releases, checks its SHA-256 and prints
the SDK root:

```sh
export VITASDK="$(scripts/ci/install-vitasdk.sh "$HOME/vitasdk-pinned")"
cmake -S . -B build-vita -G Ninja -DKISAK_PLATFORM=vita -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-vita --target KisakCOD-dedi-vpk
# -> bin/KisakCOD-dedi (ELF with symbols) and bin/KisakCOD-dedi.vpk
```

Hosted CI: the Linux workflow's **PS Vita dedicated server** job builds Debug
and RelWithDebInfo, checks the binary for fused multiply-add and NEON float
instructions, and uploads the RelWithDebInfo `.vpk` as an artifact.

## Running on the Vita (first run)

1. Install `KisakCOD-dedi.vpk` with VitaShell. The bubble has no icon yet.
2. Copy the PC game's `main/` and `zone/` folders to `ux0:data/KisakCOD/`
   (for mp_crash: `zone/english/code_post_gfx_mp.ff`, `common_mp.ff`,
   `localized_common_mp.ff`, `mp_crash.ff`, `localized_mp_crash.ff`, plus
   `main/*.iwd`). Game data stays on your devices.
3. Optional: write the server's command line to
   `ux0:data/KisakCOD/cmdline.txt`, all on one line. Without it the server
   runs `+set dedicated 1 +map mp_crash`. Quotes keep spaces in one argument.
   The [Linux guide](linux-dedicated.md) lists the settings, for example the
   retail-client ones (`sv_allowUnverifiedGuids`, `sv_allowProtocol6`).
4. Turn Wi-Fi on and start the bubble. The screen stays black: the server has
   no display. Everything it prints goes to `ux0:data/KisakCOD/console.log`.
5. Join from a PC on the same network (`/connect <vita ip>`). The Vita's
   address is in the log (`IP: ...`).
6. Hold **START + SELECT** to quit cleanly (the same as typing `quit`).

What to send back from the first runs: `console.log`, how long the map took
to load, and whether a client could join. Then time `SV_Frame` with 0, 4 and 8
bots.

## How the port works

### Toolchain: Clang compiles, VitaSDK links

`scripts/platform/vita/vita-clang.cmake` compiles every unit with Clang for
`armv7a-none-eabihf`, `-mcpu=cortex-a9 -mthumb`, against VitaSDK's newlib and
libstdc++ headers, and links with `arm-vita-eabi-g++` (VitaSDK's crt files,
linker script and stub libraries). `-Wl,-q` keeps the relocations that
`vita-elf-create` needs; `vita-make-fself` and `vita-pack-vpk` finish the
`.vpk` (`scripts/platform/vita/CMakeLists.txt`). The ABI points:

- **Floats.** Hard-float calls and scalar VFPv3 only (`-mfpu=vfpv3`), no
  fused multiply-add (`-ffp-contract=off`), signed `char`: the armv7 rules,
  so the server's movement and collision maths give the same results as on a
  PC. The whole binary, VitaSDK's libraries included, has no NEON float
  instruction (CI checks). At start-up each thread clears the FPSCR
  flush-to-zero and default-NaN bits (`Vita_SetIeeeFloatMode`); the log
  records what the system had set.
- **Thread-local storage.** VitaSDK's GCC is built without native TLS, and the
  Vita loader has no TLS segment, so Clang uses `-femulated-tls`, the same
  `__emutls_get_address` scheme as GCC's libgcc.
- **Enum size.** VitaSDK's libraries use short enums (`Tag_ABI_enum_size:
  small`); the engine keeps 4-byte enums, because its retail structures (and
  the fastfile layout checks in `db_disk_layout.h`) need them. This is safe at
  the boundaries the server uses: VitaSDK's own structure size checks
  (`ENABLE_VITASDK_BUILD_ASSERT`, on for this build) pass under Clang for
  every system header the server includes;
  libstdc++ sizes its stream flag enums to 4 bytes on purpose; the scoped
  enums (`std::chars_format`, `std::filesystem` options, `std::errc`) have a
  fixed `int` type; and the one unscoped enum in a shared structure, the
  red-black tree node colour behind `std::map`/`std::set`, is only read inside
  libstdc++ (1-byte reads of the low byte, which the engine's 4-byte writes
  set correctly on a little-endian CPU). The linker's enum-size warning is
  therefore turned off. Re-check this list when the server starts using more
  of the C++ library.
- **Fastfiles.** Pointers are 4 bytes and the CPU little-endian, so PC
  fastfiles load unchanged, as on i386 and armv7 Linux.

### Platform layer

The Vita build compiles the POSIX layer in `src/linux/` (newlib offers the
same calls as Linux for files, sockets, threads and time) and adds
`src/vita/` for what the Vita lacks. Engine sources only gained
`|| defined(__vita__)` on their existing POSIX guards, plus a Vita branch for
the challenge random number (`sceKernelGetRandomNumber`).

| Area | On the Vita |
| --- | --- |
| Virtual memory | No API for it. `src/vita/vita_mman.cpp` backs `mmap`, `mprotect`, `madvise` and `munmap` with memory blocks: a reserve allocates its full size at once (zeroed), protection changes do nothing, a decommit zeroes the range. |
| Memory budget | Engine reservations: physical memory arena 128 MB, hunk 10 MB, debug hunk 16 MB, plus small listing hunks. newlib heap 96 MB (`_newlib_heap_size_user`, default would be 128 MB), main thread stack 8 MB, network pool 1 MB. The `.vpk` asks for extended memory (`ATTRIBUTE2=12`). The laptop memory run puts mp_crash's game data at 82 MB. |
| Start-up | `Vita_Startup` (`src/vita/vita_main.cpp`): CPU to 444 MHz and bus to 222 MHz, data folder `ux0:data/KisakCOD` created and made the working directory, console log opened, network started (module, `sceNetInit` with a 1 MB pool, `sceNetCtlInit`), command line read from `cmdline.txt`. |
| Paths | `fs_basepath` defaults to `ux0:data/KisakCOD`. Paths with a device (`ux0:`) count as absolute. Directory entries carry their `SceIoStat` instead of `d_type`; the tree removal walks by path (no symbolic links on the Vita). |
| Files | newlib's `off_t` is 32-bit: positional reads and writes past 2 GiB fail instead of wrapping. The stock fastfiles and IWDs are far smaller. |
| Network | BSD sockets through newlib. No `getifaddrs`: the local addresses are loopback and the Wi-Fi address from `sceNetCtlInetGetInfo`. No `MSG_TRUNC`: a datagram that fills the receive buffer is dropped as oversize. |
| Threads | newlib pthreads. Worker threads get a 1 MB stack (the Windows default); they cannot be renamed. Three cores are reported to the engine (applications run on three of the four). |
| Time | `__rdtsc` reads `sceKernelGetProcessTimeWide` (microseconds). |
| Console | No terminal and no stdin: output goes to `console.log`, commands come from `cmdline.txt` and rcon. |
| Quit | No signals: START + SELECT queues `quit`. The server also keeps the console from auto-suspending. |
| Steam | Off. Admission is unchanged (retail rules; see the Linux guide). |

Not built for the Vita: `src/linux/sl_trace.cpp` (glibc backtraces), and the
Linux runtime self-tests, which fork and check memory protection faults the
Vita cannot raise.

## Next steps

1. First run on David's Vita (steps above): reach "Dedicated server
   initialized", load mp_crash, join from the laptop.
2. Measure `SV_Frame` with 0, 4 and 8 bots at `sv_fps 20`, and free memory
   after the map loads; then size the player cap and the newlib heap from it.
3. Commit the reservations lazily (the 16 MB debug hunk is rarely used) if
   memory is short.
4. Optimisations from the feasibility study (bots, collision, script VM).
