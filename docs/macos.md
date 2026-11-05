# macOS track

An opt-in build of the **dedicated server** (`KisakCOD-dedi`) for macOS, on
Apple silicon (arm64) and Intel Macs (x86_64). It is the first step of the Mac
plan in the project's macOS research report: get the engine compiling and
starting on a Mac in hosted CI, before any test on real Mac hardware. The
client and singleplayer are not built for macOS yet.

macOS has run no 32-bit process since 10.15, so this track sits on the 64-bit
work: the x86_64 track ([linux-x64.md](linux-x64.md)) and the aarch64 track
([linux-arm64.md](linux-arm64.md)). The defaults of every other build are
unchanged.

## Building

On a Mac with the Xcode command line tools, CMake and Ninja
(`brew install cmake ninja`):

```sh
cmake -S . -B build-macos -G Ninja \
    -DKISAK_PLATFORM=macos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-macos --target KisakCOD-dedi
```

- `CMAKE_OSX_ARCHITECTURES` is `arm64` or `x86_64` (the host's when left out).
  One architecture per build directory: a universal binary needs per-slice
  flags (the SSE options are x86_64-only), so it is not offered yet. An x86_64
  build on Apple silicon runs under Rosetta 2.
- The minimum macOS version defaults to 13.3 (`CMAKE_OSX_DEPLOYMENT_TARGET`):
  the engine uses `std::format`, whose float formatting Apple's C++ library
  offers from macOS 13.3 on.
- Apple Clang is used as is; `-fms-extensions` covers the engine's Microsoft
  spellings, as on Linux.
- Every other opt-in (client, window host, probes, movies, the Linux and
  Windows Tracy builds) is refused with `KISAK_PLATFORM=macos` for now.

Run it like the Linux server, with the game files from your own install:

```sh
bin/KisakCOD-dedi +set fs_basepath "/path/to/Call of Duty 4" +set dedicated 2 +map mp_crash
```

## How it differs from the Linux build

The Mac build uses the same POSIX platform layer as Linux (`src/linux`,
`scripts/platform/linux`). The changes are:

- **Engine guards.** The engine's `#if defined(__linux__)` tests meant "the
  native POSIX port", so each one now reads
  `defined(__linux__) || defined(__APPLE__)`. On Linux and Windows the
  preprocessed sources are unchanged (checked for every dedicated server unit).
- **Linux-only calls** in `src/linux`, each with a macOS branch:

  | Where | Linux | macOS |
  | --- | --- | --- |
  | `sys_posix.cpp` timed waits | condition variables on `CLOCK_MONOTONIC` (`pthread_condattr_setclock`) | the monotonic deadline turned into a relative timeout (`pthread_cond_timedwait_relative_np`) |
  | `net.cpp` receive | `recvfrom(MSG_TRUNC)` returns the real datagram length | `recvmsg` and its `MSG_TRUNC` flag mark an oversize datagram |
  | `sys_virtualmem.cpp` decommit | `madvise(MADV_DONTNEED)` zero-fills private pages | fresh anonymous pages mapped over the range (`MAP_FIXED`, only inside a reservation this module owns), since macOS keeps old contents after `MADV_DONTNEED` |
  | `sys_files.cpp` install path | `/proc/self/exe` | `_NSGetExecutablePath` + `realpath` |
  | `main.cpp` CPU name | `/proc/cpuinfo` | `sysctlbyname("machdep.cpu.brand_string")` |
  | `sys_memory.cpp` RAM size | `sysinfo` | `sysctlbyname("hw.memsize")` |
  | `sys_threads.cpp` thread names | `pthread_setname_np(thread, name)` | `pthread_setname_np(name)` (current thread) |
  | `sl_trace.cpp` (diagnostic) | `SYS_gettid` | `pthread_threadid_np` |

- **Headers.** macOS has no `<endian.h>` (the byte-order check uses the
  compiler's `__BYTE_ORDER__` there) and no `<malloc.h>` (`scr_yacc.cpp` takes
  `alloca` from `<alloca.h>`).
- **Build flags.** `-ffp-contract=off` on both architectures, so Apple silicon
  does not fuse multiply-adds and its movement math matches x86 (the same rule
  as the aarch64 Linux build). x86_64 keeps `-msse2 -mfpmath=sse`. Apple's
  linker strips unused code with `-dead_strip` where GNU ld uses
  `--gc-sections`; plain `char` is already signed on both Apple ABIs.
- **Tracy timer.** The server keeps Tracy compiled in (on demand), built with
  `TRACY_TIMER_FALLBACK` on macOS. Without it, Tracy stops the process at
  startup on an Intel CPU that reports no invariant TSC, as the hosted Intel
  runners (virtual machines) do; Apple silicon has no such check.
- **Build banner.** `--version` says `native macOS`, and the build string is
  `macos-arm64` or `macos-x86_64` (`-debug` for Debug builds).

## Checks

Hosted CI (`.github/workflows/build-kisakcod-macos.yaml`) builds Debug and
RelWithDebInfo for each architecture on its own runner (`macos-26` for arm64,
`macos-15-intel` for x86_64), checks that the arm64 binary contains no fused
multiply-add instructions, and runs the asset-free bootstrap test:

```sh
python3 scripts/tests/test_linux_bootstrap.py --arch macos-arm64 build-macos-bin/KisakCOD-dedi
```

That test reads the Mach-O header (64-bit executable, CPU type), checks
`--help` and `--version`, and starts the engine without game data until it
stops at the missing `fileSysCheck.cfg` and, with a generated stub tree, at the
first missing fastfile. The RelWithDebInfo binaries are kept as CI artifacts
for 14 days.

## Not covered yet

- No map has been loaded on a Mac: hosted runners have no game data. The first
  real run will be on an Intel MacBook Pro (x86_64).
- Hidden runtime differences that only game data reaches, such as the 16 KiB
  pages of Apple silicon (the virtual memory layer keeps 4 KiB commit units on
  bigger pages, tested on Linux under qemu with 16 KiB pages) and float results
  compared with x86.
- The client: the renderer path on macOS (DXVK on MoltenVK or the planned
  Vulkan back end), SDL3, OpenAL Soft, the app bundle and signing are later
  steps.
