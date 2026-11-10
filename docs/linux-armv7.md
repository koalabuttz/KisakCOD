# 32-bit Arm (armv7) track

i386 stays the shipping Linux build. This page covers the opt-in 32-bit Arm
build, started 2026-11-07 as the first step toward a PS Vita port. The Vita's
CPU is a Cortex-A9 (armv7-a), and it runs 32-bit code with 4-byte pointers, as
i386 does. So a Linux armv7 build exercises the Arm-specific code paths a Vita
build needs, on hosted CI, before any Vita SDK work starts.

Nothing on this page changes the default build, `build-linux.sh`, the other
tracks, the Windows builds or the i386 stdcall and 32-bit layout checks.

## Why armv7 is close to i386

- **Fastfiles stream unchanged.** Pointers are 4 bytes, so every streamed record
  keeps its retail offsets and size. `src/database/db_disk_layout.h` checks the
  241 streamed record types at compile time on any 32-bit target; the armv7
  build compiles, so they all match retail. No record conversion (the 64-bit
  tracks' `DB_NativeBlockSize` work) is needed.
- **The Arm fixes already exist.** The [aarch64 track](linux-arm64.md) made the
  engine build without SSE (`KISAK_ARCH_X86`, scalar skinning), made plain
  `char` signed (`-fsigned-char`) and kept Clang from fusing multiply-adds
  (`-ffp-contract=off`). armv7 uses the same flags and paths.

What is new for armv7:

- `src/universal/arch_intrin.h`: the Cortex-A9 has no architectural generic
  timer, so `__rdtsc` counts nanoseconds from `CLOCK_MONOTONIC`. Callers only
  subtract two readings or scale them by the calibrated rate, so the unit does
  not matter.
- `CPUSTRING` is `linux-armv7` (`src/universal/q_shared.h`).
- **No NEON float math in engine code.** ARMv7 NEON flushes denormals to zero,
  unlike x86 SSE and the scalar VFP unit. With `-mfpu=neon`, Clang moved
  collision traces (`CM_TraceThroughTree` and others) and `BG_CalculateWeaponPosition_Sway`,
  code client prediction shares with the server, onto NEON registers even with
  its vectorizers turned off. The toolchain file therefore targets
  `-mfpu=vfpv3` (scalar VFP, IEEE results). Hand-written NEON for client-only
  hot paths (skinning, animation blending) can opt in per file later.
- The VFP `vmla`/`vmls` instructions Clang still emits are *chained*, not fused:
  they round the product before adding, so they give the same result as a
  separate multiply and add.

## Building

On an x86 Linux host the build cross-compiles with Clang against the GNU armhf
sysroot (Debian/Ubuntu: `g++-arm-linux-gnueabihf`, and `qemu-user` to run the
result). `KISAK_LINUX_ARCH=armv7` picks
[`scripts/platform/linux/armv7-linux-gnueabihf.cmake`](../scripts/platform/linux/armv7-linux-gnueabihf.cmake);
pass `-DCMAKE_TOOLCHAIN_FILE` to use another toolchain. On an armhf host the
build is native.

```sh
cmake -S . -B build-armv7 -G Ninja -DKISAK_PLATFORM=linux -DKISAK_LINUX_ARCH=armv7 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-armv7 --target KisakCOD-dedi
```

Only `KisakCOD-dedi` builds; the configuration refuses the clients, the SP game
and the probes. On Ubuntu 24.04, `g++-multilib` (needed for i386 builds)
conflicts with the armhf cross toolchain; build armv7 and i386 in separate
containers or install one at a time.

Checks (no game data):

```sh
# Cross-compiled: run through qemu-user. On an armhf host leave --emulator out.
python3 scripts/tests/test_linux_bootstrap.py --arch armv7 \
    --emulator "qemu-arm -L /usr/arm-linux-gnueabihf" bin/KisakCOD-dedi
# Float results must match x86: no fused multiply-adds, no NEON float math.
arm-linux-gnueabihf-objdump -d bin/KisakCOD-dedi | grep -cE $'\t(vfma|vfms|vfnma|vfnms)'   # 0
arm-linux-gnueabihf-objdump -d bin/KisakCOD-dedi | grep -cE $'\tv[a-z0-9]+\.f(32|64)\tq'   # 0
```

Hosted CI: the Linux workflow's **armv7 dedicated server** job builds Debug and
RelWithDebInfo on the x86 runner, runs both objdump checks and the bootstrap
test under qemu-arm.

## State

| Step | State |
| --- | --- |
| armv7 Linux dedicated server | Builds (Debug and RelWithDebInfo) and passes the asset-free bootstrap test under qemu-arm (sandbox, 2026-11-07). Not yet run with game data. |
| Load a retail map on armv7 | Not tried. qemu-arm on the laptop or any armhf board with the game files would do it. |
| Vita dedicated server | Builds into a `.vpk` (`KISAK_PLATFORM=vita`, 2026-11-07). On a console it has run 9 h on mp_crash and taken a player (2026-11-09). See [vita.md](vita.md). |

The feasibility study, memory and CPU estimates and the staged plan are in the
project files (`vita/vita-port-feasibility.md`).
