# PS Vita dedicated server

`KISAK_PLATFORM=vita` builds the multiplayer dedicated server, `KisakCOD-dedi`,
for a hacked PS Vita (HENkaku/Ensō), packed as `KisakCOD-dedi.vpk`. It is an
opt-in track started 2026-11-07, after the [armv7 Linux build](linux-armv7.md)
proved the engine on the Vita's CPU family. Nothing here changes the Linux,
macOS or Windows builds.

**State:** the server compiles, links and packs into a `.vpk` (sandbox and
hosted CI), and boots in the Vita3K emulator: start-up, the data folder, the
log, threads and the file system work, and with no game data it stops at the
missing first zone, as the Linux build does. The first run on David's Vita
(2026-11-08) started up, logged 208 MB of main memory free, then crashed
while listing the `.iwd` files: an 8-byte store to an odd address, which the
Vita's CPU refuses and Vita3K allows (see "Alignment" below). The main
thread also had 256 KB of stack instead of 8 MB. The second run loaded
mp_crash's zones in 32 s and crashed as the game started: every `thread_local`
was one copy shared by all threads (see "Thread-local storage" below). The
third run started the game and stopped in the first script vector sum: the
script's vector constants sit unaligned in the bytecode, and float loads
(`VLDR`) fault there (see "Alignment"). All are fixed; the network, the map
and the memory budget are next checked on the console with the steps below.

The Vita build takes at most **24 players** (`sv_maxclients` 1 to 24, default
24). The server's snapshot buffers are sized by that cap; at retail's 64 they
alone would not fit in the Vita's memory (see "Memory" below).

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

What to send back from the first runs: `console.log` and whether a client
could join. The log answers the rest (next section). Then run with 0, 4 and 8
bots for a few minutes each and send that log too (bots: see
[Server-side test clients](server-test-clients.md)).

### Reading console.log

Each line starts with the seconds since launch, so the gap between
`Loading fastfile mp_crash` and the first frames is the map load time. Lines
starting `Vita` come from the Vita layer:

- `free memory at start-up`: what the system had left after loading the
  program and newlib's heap, before the engine's reservations.
- `Vita memory ...` (after start-up, then once a minute, and at a fatal
  error): game memory free in the zone arena, heap use out of its block, the
  engine's reservations and what the system still has free. An out-of-memory
  error prints it last, which shows the pool that ran out.
- `Vita load, last 60 s: busy 12.3%, slowest frame 18.2 ms, 0 frames over
  50 ms`: the share of time the main thread worked rather than waited for
  packets, its longest frame, and how many frames took longer than one server
  frame at `sv_fps 20`. The minute with the map load shows the load as its
  slowest frame. Busy near 100% or frames over 50 ms in play mean the Vita is
  not keeping up with that player count.

The `vitastats` command (console or rcon) prints both lines on demand; the
engine's `meminfo` lists the zone arena by zone.

## Testing in Vita3K

Vita3K boots the `.vpk` far enough to check start-up, files, threads and,
with game data, map loading. It does not model the Vita's memory limit (it
leaves the program's own size out of its free memory) or its CPU speed, so
the memory budget and the load figures still need the console.

In a headless Linux sandbox, the Vita3K AppImage (continuous release) works
with software OpenGL on Xvfb:

```sh
./Vita3K-x86_64.AppImage --appimage-extract && ln -s squashfs-root AppDir
Xvfb :91 -screen 0 1280x720x24 +extension GLX &
cd AppDir && DISPLAY=:91 XDG_RUNTIME_DIR=/tmp/xdg LIBGL_ALWAYS_SOFTWARE=1 \
  MESA_GL_VERSION_OVERRIDE=4.6COMPAT MESA_GLSL_VERSION_OVERRIDE=460 \
  LIBGL_DRIVERS_PATH=$PWD/lib/dri \
  ./AppRun --frontend sdl -B OpenGL /path/to/KisakCOD-dedi.vpk
```

The emulated `ux0:` is `~/.local/share/Vita3K/Vita3K/ux0`, so the log is at
`ux0/data/KisakCOD/console.log` there. Without game data, copy the stub tree
that `scripts/tests/test_linux_bootstrap.py` makes (two small `.iwd` files and
a one-line `fileSysCheck.cfg` in `main/`) to get as far as the first zone.

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
  `__emutls_get_address` scheme as GCC's libgcc. libgcc keeps one copy per
  thread only if its weak references to `pthread_cancel` and `pthread_once`
  resolve; nothing else in the link uses them, so they are pulled in with
  `--undefined`. Without them every `thread_local` (the engine's thread
  context among them) is shared by all threads: the second console run's
  database thread switched the main thread's context, and the main thread
  then crashed on another thread's empty trace data. CI fails on any weak
  `pthread_` reference left unresolved, and at start-up the server checks
  that a second thread sees its own copy, and stops if not.
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
- **Alignment.** The Cortex-A9 handles an unaligned single load or store
  (`LDR`/`STR`), but faults on the paired and multiple forms
  (`LDRD`/`STRD`/`LDM`/`STM`, and `VLDR`) at an unaligned address. Linux
  fixes these up in the kernel; the Vita does not, and Vita3K does not model
  the fault. The engine reads and writes words at any address (decompiled
  byte-buffer code), so every unit is compiled with
  `-mllvm -arm-assume-misaligned-load-store`, which stops Clang pairing two
  word accesses into one. Copies of whole 8-byte structures still use the
  paired forms; they are safe while those structures stay aligned. Float
  loads and stores (`VLDR`/`VSTR`) need 4-byte alignment on every ARMv7, so
  the script compiler and VM copy float operands out of the packed bytecode
  with `memcpy`, and on 32-bit ARM `OP_GetVector` pushes an aligned,
  reference-counted copy of the constant instead of a pointer into the code.
  A crash
  with stop reason `0x30004` (data abort) on the console is the first thing
  to check for this.
- **Main thread stack.** `sceUserMainThreadStackSize` (8 MB, in
  `vita_main.cpp`) is read only by `vita-elf-create`, so the link keeps it
  with `--undefined`; `--gc-sections` dropped it in the first build and the
  main thread got the 256 KB default.

### Platform layer

The Vita build compiles the POSIX layer in `src/linux/` (newlib offers the
same calls as Linux for files, sockets, threads and time) and adds
`src/vita/` for what the Vita lacks. Engine sources only gained
`|| defined(__vita__)` on their existing POSIX guards, plus a Vita branch for
the challenge random number (`sceKernelGetRandomNumber`).

| Area | On the Vita |
| --- | --- |
| Virtual memory | No API for it. `src/vita/vita_mman.cpp` backs `mmap`, `mprotect`, `madvise` and `munmap` with memory blocks: a reserve allocates its full size at once (zeroed), protection changes do nothing, a decommit zeroes the range. |
| Memory budget | See "Memory" below. |
| Start-up | `Vita_Startup` (`src/vita/vita_main.cpp`): CPU to 444 MHz and bus to 222 MHz, data folder `ux0:data/KisakCOD` created and made the working directory, console log opened, network started (module, `sceNetInit` with a 1 MB pool, `sceNetCtlInit`), command line read from `cmdline.txt`. |
| Paths | `fs_basepath` defaults to `ux0:data/KisakCOD`. Paths with a device (`ux0:`) count as absolute. Directory entries carry their `SceIoStat` instead of `d_type`; the tree removal walks by path (no symbolic links on the Vita). |
| Files | newlib's `off_t` is 32-bit: positional reads and writes past 2 GiB fail instead of wrapping. The stock fastfiles and IWDs are far smaller. |
| Network | BSD sockets through newlib. No `getifaddrs`: the local addresses are loopback and the Wi-Fi address from `sceNetCtlInetGetInfo`. No `MSG_TRUNC`: a datagram that fills the receive buffer is dropped as oversize. |
| Threads | newlib pthreads. Worker threads get a 1 MB stack (the Windows default); they cannot be renamed. Three cores are reported to the engine (applications run on three of the four). |
| Time | `__rdtsc` reads `sceKernelGetProcessTimeWide` (microseconds). |
| Console | No terminal and no stdin: output goes to `console.log`, commands come from `cmdline.txt` and rcon. |
| Quit | No signals: START + SELECT queues `quit`. The server also keeps the console from auto-suspending. Quit and fatal errors end the process with `sceKernelExitProcess`: newlib's `exit` frees the heap before the kernel stops the other threads, and a worker running in that gap faults (Vita3K showed it). |
| Steam | Off. Admission is unchanged (retail rules; see the Linux guide). |

### Memory

The `.vpk` asks for extended memory (`ATTRIBUTE2=12`): 365 MB of main memory
for the application. The Vita has no virtual memory, so everything below is
in memory from launch:

| Part | Size |
| --- | --- |
| Program and its static data | ~87 MB, of which `svs`, the server's snapshot buffers, is 70 MB |
| newlib heap (`_newlib_heap_size_user`) | 64 MB (newlib would take 128 MB) |
| Engine reservations (`vita_mman.cpp`) | 128 MB zone arena, 10 MB hunk, 16 MB debug hunk, small listing hunks |
| Stacks and network pool | main thread 8 MB, workers 1 MB each, network 1 MB (static) |

That leaves about 45 MB. On the console the system reported 208 MB of
main memory free once the server had started (Vita3K reports 301 MB, because
it leaves the program out). In retail `svs` is 178 MB: its rings are sized for
64 clients (64 `client_t` of 662 KB, delta-snapshot entity and client states,
and the cached snapshots behind killcam). The Vita build sets `SV_MAX_CLIENTS`
(`bg_local.h`) to 24 and the rings scale with it (`server_mp.h`): the same
history per client, for fewer clients. Every other build keeps the retail
sizes (the i386 `svs` is unchanged). The laptop memory run puts mp_crash's
zones at 82 MB of the 128 MB arena; the heap held 0.2 MB at the first zone in
Vita3K, so its size is set again from the first map loads.

Not built for the Vita: `src/linux/sl_trace.cpp` (glibc backtraces), and the
Linux runtime self-tests, which fork and check memory protection faults the
Vita cannot raise.

## Next steps

1. Fourth run on David's Vita (steps above), with the alignment, stack,
   thread-local and script vector fixes: reach "Dedicated server initialized", load mp_crash,
   join from the laptop. Expect more alignment faults; each dump's PC maps to
   source.
2. Map load in Vita3K with game data (David's laptop has Vita3K and the
   data): checks the zones load and gives the heap's use after a map.
3. Run 0, 4 and 8 bots at `sv_fps 20` and read the load lines; size the
   player cap, the heap and the zone arena from them.
4. Commit the reservations lazily (the 16 MB debug hunk is rarely used) if
   memory is short.
5. Optimisations from the feasibility study (bots, collision, script VM).
