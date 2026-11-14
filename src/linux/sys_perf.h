#pragma once

// In-game A/B measuring (docs/perf-ab.md): the console commands perf_ab,
// perf_sample and perf_stop, and the counters engine code feeds them.
//
// The counters and the sleep live in this header, not in sys_perf.cpp, so the
// renderer can use them in every program it is linked into (the probes and
// test programs list their platform files one by one).
//
// The Windows build includes it too (src/win32 lists sys_perf.cpp): there the
// clock is std::chrono::steady_clock (QueryPerformanceCounter) and there is no
// sleep, since r_gpuSyncSleep is not in the Windows build.

#include <atomic>
#include <cstdint>
#if defined(_WIN32)
#include <chrono>
#else
#include <time.h>
#endif

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
#if !defined(_WIN32)
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
#endif

// ---------------------------------------------------------------------------
// Probes: pieces of the game that are timed while a perf_ab or perf_sample run
// is active. Outside a run a probe costs one load and one branch.
// ---------------------------------------------------------------------------

inline constexpr uint32_t SYS_PERF_WORKER_TYPE_SLOTS = 24; // at least WRKCMD_COUNT
inline constexpr uint32_t SYS_PERF_LOCK_SLOTS = 40;        // at least CRITSECT_COUNT

enum SysPerfProbe : uint32_t
{
    SYS_PERF_PROBE_SERVER_FRAME,       // one 20 Hz frame of a listen server, with its sends
    SYS_PERF_PROBE_HUD,                // CG_Draw2D
    // Parts of a server frame (game_mp, server_mp), so the server frame's own
    // time can be split. They sit inside SYS_PERF_PROBE_SERVER_FRAME.
    SYS_PERF_PROBE_SERVER_BOTS,        // SV_UpdateBots: the bots' moves (their ClientThink)
    SYS_PERF_PROBE_SERVER_TRIGGERS,    // G_RunFrame: players touching triggers, trigger notifies and their scripts
    SYS_PERF_PROBE_SERVER_ANIM,        // G_RunFrame: the two animation passes over every entity
    SYS_PERF_PROBE_SERVER_SCRIPTS,     // G_RunFrame: Scr_IncTime, the script threads whose wait ended
    SYS_PERF_PROBE_SERVER_ENTITIES,    // G_RunFrame: G_RunFrameForEntity for every entity
    SYS_PERF_PROBE_SERVER_CLIENT_END,  // G_RunFrame: ClientEndFrame for every player
    SYS_PERF_PROBE_SERVER_SEND,        // SV_SendClientMessages: snapshots to the clients
    SYS_PERF_PROBE_WAIT_RENDERER,      // main thread asleep until the render thread takes the frame
    SYS_PERF_PROBE_WAIT_WORKERS_ALL,   // ... until every front-end worker command is done
    SYS_PERF_PROBE_WAIT_WORKERS_OTHER, // ... in any other wait that runs worker commands
    SYS_PERF_PROBE_WAIT_WORKER_TYPE,   // + WorkerCmdType: ... until the commands of that type are done
    SYS_PERF_PROBE_LOCK = SYS_PERF_PROBE_WAIT_WORKER_TYPE + SYS_PERF_WORKER_TYPE_SLOTS,
                                       // + critical section: any thread getting into it
    SYS_PERF_PROBE_COUNT = SYS_PERF_PROBE_LOCK + SYS_PERF_LOCK_SLOTS
};

struct SysPerfProbeSlot
{
    std::atomic<uint32_t> ns;    // wraps; the reader takes differences once a frame
    std::atomic<uint32_t> calls; // wraps
    std::atomic<uint32_t> maxNs; // the reader resets it
};

inline SysPerfProbeSlot g_sysPerfProbes[SYS_PERF_PROBE_COUNT];
inline std::atomic<bool> g_sysPerfProbesOn;

inline bool Sys_PerfProbesOn()
{
    return g_sysPerfProbesOn.load(std::memory_order_relaxed);
}

inline uint64_t Sys_PerfNowNs()
{
#if defined(_WIN32)
    // Never 0 in practice (the counter starts at boot), which SysPerfScope
    // takes as "not timing".
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

inline void Sys_PerfProbeAdd(uint32_t probe, uint64_t ns)
{
    if (probe >= SYS_PERF_PROBE_COUNT)
        return;
    const uint32_t ns32 = ns > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ns;
    SysPerfProbeSlot &slot = g_sysPerfProbes[probe];
    slot.ns.fetch_add(ns32, std::memory_order_relaxed);
    slot.calls.fetch_add(1, std::memory_order_relaxed);
    // Two threads can race here and lose the larger value; the longest call
    // is a hint, not a sum.
    if (ns32 > slot.maxNs.load(std::memory_order_relaxed))
        slot.maxNs.store(ns32, std::memory_order_relaxed);
}

// Times its scope into one probe while a run is active.
class SysPerfScope
{
public:
    explicit SysPerfScope(uint32_t probe) : probe_(probe), startNs_(Sys_PerfProbesOn() ? Sys_PerfNowNs() : 0) {}
    ~SysPerfScope()
    {
        if (startNs_)
            Sys_PerfProbeAdd(probe_, Sys_PerfNowNs() - startNs_);
    }
    SysPerfScope(const SysPerfScope &) = delete;
    SysPerfScope &operator=(const SysPerfScope &) = delete;

private:
    uint32_t probe_;
    uint64_t startNs_;
};

// Registers the commands. Main thread, after the command system is up.
void Sys_PerfInit();
// Once per main-loop frame, on the main thread.
void Sys_PerfFrame();
