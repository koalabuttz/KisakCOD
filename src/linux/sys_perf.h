#pragma once

// In-game A/B measuring (docs/perf-ab.md): the console commands perf_ab,
// perf_sample and perf_stop, and the counters engine code feeds them.
//
// The counters and the sleep live in this header, not in sys_perf.cpp, so the
// renderer can use them in every program it is linked into (the probes and
// test programs list their platform files one by one).

#include <atomic>
#include <cstdint>
#include <time.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

enum SysPerfCounter
{
    SYS_PERF_GPU_WAIT_MAIN,    // main thread waiting for the GPU fence (R_SyncGpu)
    SYS_PERF_GPU_WAIT_BACKEND, // render thread waiting for it (RB_AdaptiveGpuSyncFinal)
    SYS_PERF_COUNTER_COUNT
};

// Microseconds, wrapping. Each counter is fed by one thread.
inline std::atomic<uint32_t> g_sysPerfCounters[SYS_PERF_COUNTER_COUNT];

inline void Sys_PerfAddMicroseconds(SysPerfCounter counter, uint32_t microseconds)
{
    g_sysPerfCounters[counter].fetch_add(microseconds, std::memory_order_relaxed);
}

// Sleeps the calling thread for about this long. On Linux the thread's timer
// slack is lowered first: with the default 50 us slack a 50 us sleep can take
// 100 us or more.
inline void Sys_PerfSleepMicroseconds(uint32_t microseconds)
{
#if defined(__linux__)
    static thread_local bool s_slackLowered;
    if (!s_slackLowered)
    {
        s_slackLowered = true;
        prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL);
    }
#endif
    struct timespec request;
    request.tv_sec = (time_t)(microseconds / 1000000u);
    request.tv_nsec = (long)(microseconds % 1000000u) * 1000L;
    // An interrupted sleep is not resumed: every caller polls again anyway.
    nanosleep(&request, nullptr);
}

// Registers the commands. Main thread, after the command system is up.
void Sys_PerfInit();
// Once per main-loop frame, on the main thread.
void Sys_PerfFrame();
