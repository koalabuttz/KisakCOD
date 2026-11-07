#pragma once

// CPU intrinsics the engine uses directly, per target architecture.
//
// The engine was written for x86 and calls a few intrinsics on its own:
// __rdtsc for profiling and timing, and SSE for float-to-int snapping.
// KISAK_ARCH_X86 tells shared code which spelling to use. On 64-bit Arm
// (aarch64 Linux, Windows on Arm) the SSE paths take portable fallbacks and
// __rdtsc reads the architectural virtual counter instead of the TSC. Callers
// only ever subtract two readings or scale them by a calibrated rate
// (msecPerRawTimerTick), which both counters support.

#include <math.h>
#include <stdint.h>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define KISAK_ARCH_X86 1
#include <xmmintrin.h> // SSE
#if defined(__linux__) || defined(__APPLE__) || defined(__vita__)
#include <x86intrin.h> // __rdtsc, _BitScanReverse
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define KISAK_ARCH_X86 0
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
inline unsigned __int64 __rdtsc()
{
    // CNTVCT_EL0, encoded as winnt.h's ARM64_SYSREG(3, 3, 14, 0, 2) spells it
    // (ARM64_CNTVCT); winnt.h is not included here.
    return static_cast<unsigned __int64>(_ReadStatusReg(0x5F02));
}
#else
inline uint64_t __rdtsc()
{
    uint64_t ticks;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
}
#endif
#elif defined(__arm__) && defined(__vita__)
// PS Vita (Cortex-A9): the process clock counts microseconds and costs one
// system call, like the Linux armv7 clock_gettime path below.
#define KISAK_ARCH_X86 0
#include <psp2/kernel/processmgr.h>
inline uint64_t __rdtsc()
{
    return static_cast<uint64_t>(sceKernelGetProcessTimeWide());
}
#elif defined(__arm__)
// 32-bit Arm (armv7, the PS Vita's Cortex-A9). The A9 has no architectural
// generic timer, so count nanoseconds from the monotonic clock instead.
#define KISAK_ARCH_X86 0
#include <time.h>
inline uint64_t __rdtsc()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}
#else
#error "arch_intrin.h: unsupported target architecture (expected x86, x86_64, 64-bit Arm or armv7)"
#endif
