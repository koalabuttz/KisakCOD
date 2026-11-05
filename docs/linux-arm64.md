# 64-bit Arm (arm64) track

i386 stays the shipping Linux build. This page covers the opt-in track toward
64-bit Arm builds, started 2026-11-05: **aarch64 Linux** first, then **Windows on
Arm (ARM64)**. It builds on the [x86_64 track](linux-x64.md): every 64-bit fix
made there (fastfile record conversion, script VM widths, `sizeof` instead of
literal sizes) applies unchanged, because aarch64 Linux uses the same LP64 data
model and struct layout rules as x86_64 Linux. What is new here is everything
that assumed an *x86* CPU rather than a 32-bit one.

Nothing on this page changes the default build, `build-linux.sh`, the x86_64
track, the Windows builds or the i386 stdcall and 32-bit layout checks.

The track is staged like the x86_64 one:

1. **Census** (this page): what stops an arm64 build, Linux and Windows.
2. **aarch64 Linux dedicated server** (this page, first step done): builds,
   boots without game data and passes the record conversion test. Loading a
   retail map has not been tried on Arm yet.
3. **aarch64 Linux client** (builds; see [below](#building-the-aarch64-client)):
   `KisakCOD-mp` builds for aarch64 and passes the client's asset-free checks.
   It has not drawn a frame yet: no aarch64 DXVK Native library has been built
   and no Arm machine with game data has run it.
4. **Windows ARM64** (build check, see [below](#building-for-windows-arm64)):
   MSVC ARM64 build of sp, mp and dedi that starts up to the missing-game-data
   error on GitHub's Windows on Arm runner. Not yet run with game data.

## Building the aarch64 dedicated server

On an Arm Linux machine (Raspberry Pi 5, Ampere, Apple silicon under Asahi or a
VM, GitHub's `ubuntu-24.04-arm` runner) the build is native:

```sh
cmake -S . -B build-arm64 -G Ninja -DKISAK_PLATFORM=linux -DKISAK_LINUX_ARCH=aarch64 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-arm64 --target KisakCOD-dedi
```

On an x86 Linux host the same command cross-compiles: `KISAK_LINUX_ARCH=aarch64`
picks [`scripts/platform/linux/aarch64-linux-gnu.cmake`](../scripts/platform/linux/aarch64-linux-gnu.cmake),
which drives Clang with `--target=aarch64-linux-gnu` against the GNU cross
sysroot. Install it first (Debian/Ubuntu: `g++-aarch64-linux-gnu`; `qemu-user` to
run the result). Pass `-DCMAKE_TOOLCHAIN_FILE` to use another toolchain.

The configuration refuses the SP game and the probes for now; `KisakCOD-dedi`
and, with the options below, the multiplayer client build.

Checks (no game data):

```sh
# Cross-compiled: run through qemu-user. On an Arm host leave --emulator out.
python3 scripts/tests/test_linux_bootstrap.py --arch aarch64 \
    --emulator "qemu-aarch64 -L /usr/aarch64-linux-gnu" bin/KisakCOD-dedi
python3 scripts/tests/test_db_record_conversion.py --arch aarch64
```

`qemu-aarch64 -p 16384` (or `65536`) runs the binary with 16 or 64 KiB pages,
which is how the page-size work below was checked.

Hosted CI: the Linux workflow's **aarch64 dedicated server** job builds Debug and
RelWithDebInfo natively on `ubuntu-24.04-arm` (free for public repositories) and
runs both checks. It does not load a map: there is no game data in CI.

## Building the aarch64 client

The multiplayer client takes the same options as the x86_64 one
([linux-mp-client.md](linux-mp-client.md#building)) plus
`-DKISAK_LINUX_ARCH=aarch64`, with aarch64 builds of SDL3 3.2.10 or newer,
OpenAL Soft and (for movies) FFmpeg. DXVK 2.7.1 has Arm code paths but has not
been built for aarch64 here yet, so the checks so far link the device probe's
D3D9 stand-in, as the x86 CI jobs do.

Checks (no game data, no GPU):

```sh
# Cross-compiled: run through qemu-user. On an Arm host leave --emulator out.
python3 scripts/tests/test_linux_mp_client.py --binary bin/KisakCOD-mp \
    --emulator "qemu-aarch64 -L /usr/aarch64-linux-gnu"
bin/KisakCOD-skin-scalar-test
```

Hosted CI: the **aarch64 multiplayer client** job builds the client natively on
`ubuntu-24.04-arm` against aarch64 SDL3 and the D3D9 and OpenAL stand-ins, then
runs the game window test, the client's start from game-data-free trees, the
Bink shim test and the scalar skinning test. It also fails if the client
contains fused multiply-add instructions.

### Skinning

`r_model_skin_sse.cpp` (SSE and MMX) compiles empty off x86, `sys_info.SSE` is
false on Arm, and skinned models go through the engine's scalar code
(`R_SkinXSurfaceSkinned`), the same path x86 takes with `r_sse_skinning 0`.
`tests/skinning/skin_scalar_test.cpp` runs that code on generated surfaces and
bones and checks two things: its output bytes hash to the same value on i386,
x86_64 and aarch64, and on x86 it matches the SSE path (positions to float
rounding, packed normals and tangents to one step, everything else exactly).
With multiply-add fusion turned back on, the aarch64 hash changes, which is the
check that `-ffp-contract=off` is in force.

## What had to change

The dedicated server needed far less than the x86_64 step did, because the
64-bit work was already done.

| Area | x86 assumption | arm64 change |
| --- | --- | --- |
| Compiler flags | `-msse2 -mfpmath=sse` on every Linux target | `KISAK_LINUX_SIMD_OPTIONS` holds them on x86 and is empty on Arm |
| `char` | signed on x86 (and on Windows on Arm) | Arm Linux defaults to unsigned: `-fsigned-char` |
| Float results | x86 has no fused multiply-add without `-mfma`, so `a * b + c` rounds twice | Arm fuses it by default; `-ffp-contract=off` keeps x86 results (client prediction replays server movement math). The built server contains no fused multiply-add instructions. |
| Intrinsics | `qcommon.h` included `<xmmintrin.h>` and `<x86intrin.h>` | [`universal/arch_intrin.h`](../src/universal/arch_intrin.h): SSE on x86 only; `KISAK_ARCH_X86` for shared code |
| `__rdtsc` | x86 time-stamp counter, used for profiling and the timer calibration | Arm's virtual counter (`cntvct_el0`; `_ReadStatusReg(ARM64_CNTVCT)` under MSVC) under the same name; `Posix_RawTimerTicks` now always uses it so the calibration matches |
| `SnapFloatToInt` | `_mm_cvtss_si32` | `lrintf` (same round-to-nearest-even; out-of-range values saturate on Arm instead of giving `INT_MIN`) |
| Memory arena | 512 MB only `#if __x86_64__` | `#if __LP64__`; Arm got the 32-bit 128 MB size before |
| Page size | the hunk commits and decommits 4 KiB pages | see below |
| `_BitScanReverse` | Clang's builtin declared on x86_64 only | declared on every LP64 target |
| Build banner | `CPUSTRING` | `linux-arm64`, and `win-arm64` for Windows |

### Pages bigger than 4 KiB

Arm Linux kernels may use 16 KiB pages (Asahi Linux on Apple silicon, some
Android-derived kernels) or 64 KiB pages (some server distributions). The engine
was written for 4 KiB pages: the hunk commits and decommits 4 KiB-aligned ranges
through `VirtualAlloc`/`VirtualFree`, and the Linux emulation in
`src/linux/sys_virtualmem.cpp` rounded those ranges out to host pages. On a
16 KiB host, decommitting the hunk's temp area would therefore zero and unmap
live data that shares a host page with it.

On hosts with pages over 4 KiB each reservation now keeps one commit flag per
4 KiB unit. A host page stays mapped while any of its units is committed, and
decommitting the other units only zeroes them, which is what Windows hands back
on a later commit. 4 KiB hosts (every x86 machine, most Arm distributions) take
the old path unchanged. The virtual-memory self-test has a new case for this;
with the old code it crashes at 16 KiB pages, with the new code it passes at 4,
16 and 64 KiB under qemu.

## Building for Windows ARM64

With Visual Studio 2022 and its ARM64 build tools (the "MSVC v143 - VS 2022 C++
ARM64 build tools" component), on any Windows machine:

```powershell
cmake -S . -B build-arm64 -G "Visual Studio 17 2022" -A ARM64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 --config Release --target KisakCOD-mp KisakCOD-sp KisakCOD-dedi
```

It is the x64 build (`KISAK_WIN64`: OpenAL Soft instead of Miles, no Steam,
movies only with `KISAK_WIN64_CINEMATICS`) with these differences, keyed on
`KISAK_WIN_ARM64`:

- **No D3DX9.** The DirectX SDK has no ARM64 D3DX library, so ARM64 links
  [`win32/win_d3dx_unavailable.cpp`](../src/win32/win_d3dx_unavailable.cpp)
  instead. Its D3DX entry points fail with an error, which only matters when a
  map loads from raw assets (`useFastFile 0`) and its shaders have to be
  compiled from HLSL. Fastfile loading, the normal path, never calls D3DX. The
  D3DX headers are still needed.
- **No SSE**: skinning takes the scalar path, as on Arm Linux.
- **No `cpuid`**: the Intel-only physical core count is skipped. The engine
  already reports a fixed CPU vendor on every Windows build, so it never ran.
- **Stack traces** in assert messages use `RtlCaptureStackBackTrace`, as on x64.
- `CPUSTRING` is `win-arm64`, and the linker machine is `ARM64`.

Hosted CI: the Windows workflow's **MSVC ARM64 build** job builds the three
games natively on `windows-11-arm`, checks that they are ARM64 executables and
that no engine object file contains a fused multiply-add instruction (the same
rule as `-ffp-contract=off` on Linux), and runs the start-up check the 32-bit and
x64 jobs run. Windows x64 binaries also run on Windows on Arm through
emulation, so the native build is about speed, not reach.

## Census: Linux client (step 3)

From a trial compile of the client sources and a look at the dependencies:

- **SSE skinning** (done, [above](#skinning)). `r_model_skin.h` included
  `<mmintrin.h>` and `r_model_skin_sse.cpp` is SSE/MMX code. This is the only
  intrinsic code in the client.
- **DXVK Native.** 2.7.1, the pinned version, has `DXVK_ARCH_ARM64` guards in
  `src/util/util_bit.h` and an Arm spin-lock path in `sync_spinlock.h`, so it is
  meant to build for aarch64. Not built here yet; the next client step is to
  build it (natively on the Arm runner, or cross with Meson) and run the device
  probe on lavapipe.
- **SDL3, OpenAL Soft, FFmpeg**: all build for aarch64 Linux (distribution
  packages exist).
- **Steam**: the Linux Steam client is x86 only, and Linux has no ticket
  provider anyway ([status](status.md)), so nothing changes.
- **Memory ordering.** Arm reorders memory more freely than x86. The platform
  layer uses `__atomic` builtins, but engine code that hands work between
  threads through plain `volatile` flags could misbehave under load. Watch for
  it in the first multi-threaded client runs; the dedicated server mostly runs
  on one thread.

## Census: Windows ARM64 (step 4)

The census below led to the [ARM64 build](#building-for-windows-arm64); it is
kept as the record of what was found.

A syntax-only compile of every `KisakCOD-mp` source with llvm-mingw's
`aarch64-w64-mingw32` target (2026-11-05, against the MinGW configuration's
compile commands) fails in 17 of 477 files:

- 11 are the pointer-to-`int` casts that Clang treats as errors on every 64-bit
  Windows target and MSVC x64 only warns about. The MSVC x64 build already
  compiles them, so they are not Arm work.
- 5 include `r_model_skin.h` (MMX, as on Linux above).
- `win32/win_configure.cpp` uses `__cpuidex` for CPU detection; ARM64 needs
  `IsProcessorFeaturePresent` or a fixed answer.
- `CPUSTRING` had no ARM64 value (fixed here).

Beyond compiling, the Windows build has x64-specific pieces that need an ARM64
branch:

- **D3D9** itself is fine: Windows 11 on Arm ships a native ARM64 `d3d9.dll`.
- **D3DX9** (June 2010 DirectX SDK) has no ARM64 library. Only
  `r_material_load_obj.cpp` (compiling loose HLSL, which fastfile builds never
  do) calls it; `r_init.cpp` only has its error strings. The Linux client already
  builds without that file; ARM64 can do the same, or call `d3dcompiler_47`,
  which does ship for ARM64.
- **Miles, Bink and the bundled Steam library** are 32-bit only, as on x64: the
  existing `KISAK_WIN64` route (OpenAL Soft, the FFmpeg Bink shim, no Steam)
  covers ARM64 because it keys on pointer size, but `KISAK_WIN_ARCH` in
  `scripts/pre_build.cmake` (library folder and linker machine) only knows x86
  and x64, and the D3DX library it picks has no ARM64 build.
- **Code guarded by `_M_X64`** that means "64-bit Windows" must also accept
  `_M_ARM64`: the skinning switch, `DoStackTrace`.
- **Float contraction**: check that MSVC ARM64 does not fuse multiply-adds under
  the project's `/fp` setting (disassemble for `fmadd`), for the same prediction
  reason as Linux.
- **CI**: GitHub's `windows-11-arm` runner is free for public repositories; a
  cross-compile from `windows-latest` with `-A ARM64` also works for a build
  check. Windows x64 binaries already run on Windows on Arm through emulation,
  so a native build is about speed, not reach.

## Open

- Load a retail map on an Arm machine (no Arm hardware with game data has been
  used yet).
- Build DXVK Native for aarch64 and run the device probe and the client on a
  real Vulkan driver.
- Run the Windows ARM64 build with game data on a Windows on Arm machine.
- Windows ARM64 movies (`KISAK_WIN64_CINEMATICS` with an ARM64 FFmpeg).
- The virtual-memory self-test's checks of 32-bit address-space limits fail on
  every 64-bit build (x86_64 too); `run_linux_checks.py` only runs it as i386.
