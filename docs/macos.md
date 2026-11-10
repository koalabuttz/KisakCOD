# macOS track

An opt-in build of the **dedicated server** (`KisakCOD-dedi`) for macOS, on
Apple silicon (arm64) and Intel Macs (x86_64). It is the first step of the Mac
plan in the project's macOS research report: get the engine compiling and
starting on a Mac in hosted CI, before any test on real Mac hardware. The
multiplayer client (`KisakCOD-mp`) also builds, against the CI stand-ins only
(see [Multiplayer client](#multiplayer-client)); singleplayer is not built
for macOS yet.

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
- The multiplayer client's opt-ins (`KISAK_BUILD_LINUX_NATIVE_D3D9`,
  `KISAK_BUILD_LINUX_MP_CLIENT` and its movies, `KISAK_LINUX_CINEMATICS`) and
  the Vulkan window probe (`KISAK_BUILD_VULKAN_PROBE`) are accepted; see below.
  Every other opt-in (window host, the D3D9 and world probes, singleplayer,
  Steam, the Linux and Windows Tracy builds) is refused with
  `KISAK_PLATFORM=macos` for now.

Run it like the Linux server, with the game files from your own install:

```sh
bin/KisakCOD-dedi +set fs_basepath "/path/to/Call of Duty 4" +set dedicated 2 +map mp_crash
```

## Multiplayer client

`KisakCOD-mp` builds with the same options as the 64-bit Linux client
([linux-x64.md](linux-x64.md)): SDL3 for the window and input, a D3D9 library
behind the renderer boundary (`KISAK_DXVK_NATIVE_LIBRARY`, with the DXVK
Native 2.7.1 headers) and an OpenAL library (`KISAK_OPENAL_LIBRARY`, headers
from `brew install openal-soft`). There is no D3D9 library for macOS yet (DXVK
on MoltenVK or the planned Vulkan back end), so CI links the stand-ins that
answer "no device" and "no sound device", and the client can only show that it
starts and stops cleanly without game data.

```sh
cmake -S . -B build-macos-client -G Ninja \
    -DKISAK_PLATFORM=macos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON \
    -DKISAK_DXVK_NATIVE_INCLUDE_DIR=/path/to/staged/dxvk \
    -DKISAK_DXVK_NATIVE_LIBRARY=/path/to/libd3d9.dylib \
    -DKISAK_BUILD_LINUX_MP_CLIENT=ON \
    -DKISAK_OPENAL_LIBRARY=/path/to/libopenal.dylib \
    -DKISAK_OPENAL_INCLUDE_DIR="$(brew --prefix openal-soft)/include" \
    -DSDL3_DIR=/path/to/SDL3/lib/cmake/SDL3
cmake --build build-macos-client --target KisakCOD-mp
```

- The client links with `-dead_strip` and no export list: Mach-O's two-level
  namespace already keeps the engine's names from binding to a library's.
- The game window test replaces `SDL_CreateWindowWithProperties` by defining
  it in the test and reaching SDL's own through `dlsym(RTLD_NEXT)`, since
  Apple's linker has no `--wrap`.
- The client keeps its configs and player profiles in
  `~/Library/Application Support/KisakCOD`, where Mac apps keep per-user data
  (`XDG_DATA_HOME` is ignored on macOS); `+set fs_homepath <folder>` chooses
  another, as on Linux.
- Movies: `-DKISAK_LINUX_CINEMATICS=ON` builds the Bink shim
  ([linux-cinematics.md](linux-cinematics.md)) as on Linux. It needs
  libavcodec and libavutil, found with pkg-config: Homebrew's `ffmpeg`, or a
  minimal LGPL build with only the Bink decoders, as CI makes:

  ```sh
  ./configure --prefix="$PWD/../ffmpeg-sdk" --install-name-dir='@rpath' \
      --extra-cflags=-mmacosx-version-min=13.3 --extra-ldflags=-mmacosx-version-min=13.3 \
      --enable-shared --disable-static --disable-programs --disable-doc \
      --disable-everything --disable-autodetect --disable-network \
      --disable-avformat --disable-avdevice --disable-avfilter \
      --disable-swscale --disable-swresample --disable-x86asm \
      --enable-decoder=bink,binkaudio_rdft,binkaudio_dct
  make && make install
  PKG_CONFIG_PATH="$PWD/../ffmpeg-sdk/lib/pkgconfig" cmake ... -DKISAK_LINUX_CINEMATICS=ON
  ```

  RAD's `radbase.h` takes its Macintosh branch on Apple compilers (it
  describes a 68K or PowerPC Mac); nothing the shim or `r_cinematic.cpp` uses
  depends on that branch. The default Mac file system ignores letter case, so the
  shim opens a movie path as the engine spells it, with `/` for `\`, and
  needs none of the case-matching walk it does on Linux.

### App bundle

`scripts/platform/macos/make_app_bundle.py` packs a built client into
`KisakCOD-mp.app`:

```sh
python3 scripts/platform/macos/make_app_bundle.py \
    --executable bin/KisakCOD-mp --output dist \
    --library /path/to/SDL3/lib/libSDL3.0.dylib \
    --library /path/to/libd3d9.dylib --library /path/to/libopenal.dylib \
    --library /path/to/ffmpeg/lib/libavcodec.61.dylib \
    --library /path/to/ffmpeg/lib/libavutil.59.dylib
```

- The libraries go in `Contents/Frameworks` under their `@rpath/` install
  names. The executable keeps one run path, `@executable_path/../Frameworks`;
  the build's own folders are removed. The script stops if a library the
  executable needs is missing, one passed is not needed, or something outside
  the bundle and the system would be loaded.
- `Info.plist` names the bundle (`io.github.kisakcod.mp` unless
  `--bundle-id` says otherwise) and the minimum macOS (13.3).
- It is signed ad hoc, which Apple silicon needs to run it. That is not a
  release signature: Gatekeeper still asks on another Mac. Signing with a
  Developer ID and notarising are later steps.
- The game files stay outside the bundle: start it with
  `+set fs_basepath "/path/to/Call of Duty 4"`, for example
  `KisakCOD-mp.app/Contents/MacOS/KisakCOD-mp +set fs_basepath ...`.

#### Universal bundle

One bundle for both kinds of Mac holds an arm64 and an x86_64 slice in the
executable and in each library. Running the macOS workflow by hand with
`universal` ticked builds the client RelWithDebInfo on both runners, packs each
bundle as above, merges them with `lipo -create`, signs the result ad hoc
again, runs the client's fail-closed checks from it (the x86_64 slice under
Rosetta 2 when the runner has it) and writes a size table to the run's
summary: each bundle as built, stripped (`strip -S -x`) and zipped. The
libraries in it are the CI ones, so the D3D9 and OpenAL stand-ins weigh almost
nothing; a release would carry its real graphics and sound libraries instead.

## Vulkan window probe

The shared core of the planned Vulkan back end and its window probe
([vulkan-core.md](vulkan-core.md)) build and run on macOS through MoltenVK, the
Vulkan-on-Metal layer. With Homebrew's `vulkan-headers`, `vulkan-loader` and
`molten-vk`:

```sh
cmake -S . -B build-macos-vulkan -G Ninja \
    -DKISAK_PLATFORM=macos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DKISAK_BUILD_VULKAN_PROBE=ON \
    -DKISAK_VULKAN_INCLUDE_DIR="$(brew --prefix vulkan-headers)/include" \
    -DSDL3_DIR=/path/to/SDL3/lib/cmake/SDL3
cmake --build build-macos-vulkan --target KisakCOD-vulkan-probe
SDL_VULKAN_LIBRARY="$(brew --prefix vulkan-loader)/lib/libvulkan.1.dylib" \
    bin/KisakCOD-vulkan-probe --resize --check-capture
```

- The probe does not link the loader: SDL opens it at run time, by bare name.
  On Apple silicon Homebrew's prefix is not on the default library path, so
  point `SDL_VULKAN_LIBRARY` at it (and add `$(brew --prefix)/lib` to
  `DYLD_FALLBACK_LIBRARY_PATH` for the validation layer).
- The core already turns on portability enumeration and the portability
  subset, which MoltenVK needs.
- Hosted Intel runners are VMs whose paravirtualized Metal device aborts when
  MoltenVK 1.4.2 sizes Metal argument buffers while creating the instance
  (inside `-[MTLDevice newArgumentEncoderWithArguments:]`), so CI runs the
  x86_64 probe with `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`. Apple silicon
  runners and real Macs do not need it.

### Vulkan back end

The rest of the Vulkan back end builds on macOS too: the Legacy shader
translator, the Legacy adapter ([vulkan-legacy-adapter.md](vulkan-legacy-adapter.md))
and its D3D9 front ([vulkan-d3d9-front.md](vulkan-d3d9-front.md)). Add
`-DKISAK_BUILD_VULKAN_SHADERS=ON -DKISAK_BUILD_VULKAN_LEGACY=ON` and the native
D3D9 boundary's options (`-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON`,
`-DKISAK_DXVK_NATIVE_INCLUDE_DIR`, headers only) to the configure line above.
CI runs the same checks as the Linux lavapipe job through MoltenVK, in a Cocoa
window on each runner: `test_vulkan_legacy_shaders.py`,
`test_vulkan_legacy_adapter.py` and `test_vulkan_d3d9_front.py`, with
`--validation auto`.

The client job also configures `KisakCOD-mp` with
`-DKISAK_BUILD_VULKAN_BACKEND=ON`, so it carries `r_backend vulkan` next to the
default D3D9 path. Nothing on the runners draws the game itself: that needs
the game's files, so the first `+set r_backend vulkan` run of a map on a Mac
waits for a Mac with the game installed. Such a run also needs the Vulkan
loader and MoltenVK where SDL finds them (`SDL_VULKAN_LIBRARY`); the app
bundle does not carry them yet.

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
multiply-add instructions, and runs the asset-free checks:

```sh
python3 scripts/tests/run_linux_checks.py --arch macos-arm64 \
    --build-dir build-macos --binary bin/KisakCOD-dedi --cxx clang++
```

- **bootstrap** reads the Mach-O header (64-bit executable, CPU type), checks
  `--help` and `--version`, and starts the engine without game data until it
  stops at the missing `fileSysCheck.cfg` and, with a generated stub tree, at
  the first missing fastfile.
- **runtime** builds `src/linux/sys_posix.cpp` and the platform wrappers with
  the build's own flags and runs their self-tests: events and timed waits
  (the macOS relative wait), threads, the render-thread handshakes, console
  input through a pipe, files and the command line.
- **net_deadline** and **udp** run the network layer against real loopback
  sockets, including an oversize datagram (the `recvmsg` path).
- **sys_fileio** and **sys_virtualmem** run the file and virtual-memory
  self-tests, the latter on Apple silicon's 16 KiB pages (decommit by
  remapping).

The other `run_linux_checks.py` checks are i386-only and stay on Linux. The
RelWithDebInfo binaries are kept as CI artifacts for 14 days.

A second job per architecture builds the multiplayer client against the pinned
SDL3 (built from source), the D3D9 and OpenAL stand-ins and, for movies, a
minimal FFmpeg n7.1.5 built from source with only the Bink decoders. It checks
with `otool -L` that the client needs only those libraries and the system ones
(and, on arm64, that it has no fused multiply-adds), then runs the game window
test, `scripts/tests/test_linux_mp_client.py --native-os macOS` (the client's
fail-closed start from temporary trees with no game data), the Bink shim test
(made-up movies, and libavcodec's Bink decoders opened as the shim opens them;
no game movie) and the skinning tests. Last it packs the client into
`KisakCOD-mp.app`, checks the signature, the `Info.plist` and the single run
path, and runs the fail-closed checks again from the bundle with no library
path set.

A third job per architecture builds the Vulkan window probe against Homebrew's
MoltenVK and runs `scripts/tests/test_vulkan_window_probe.py --video-driver
cocoa --validation auto` in a real window. On both runners (2026-11-07,
MoltenVK 1.4.2, Apple's paravirtualized GPU) it lists the device, fails closed
with no driver, and presents 121 frames through a resize with the cleared
colour read back exactly and no validation errors (validation with
synchronisation checks was on). The GPU timestamps read zero on these VMs, so
timings mean nothing there.

## Not covered yet

- No map has been loaded on a Mac: hosted runners have no game data. The first
  real run will be on an Intel MacBook Pro (x86_64).
- Hidden runtime differences that only game data reaches, such as the 16 KiB
  pages of Apple silicon under a real map load (the virtual-memory self-test
  covers the layer itself) and float results compared with x86.
- The client has drawn nothing on a Mac yet. The Vulkan back end is linked
  into it and its checks pass on Metal through MoltenVK
  ([above](#vulkan-back-end)), but a map drawn with `r_backend vulkan` needs
  the game's files. Sound through OpenAL Soft, a game movie played
  through the Bink shim, and a signed, notarised app are later steps too.
- No run on a real Mac's GPU yet: the hosted runners' GPU is a virtual one.
