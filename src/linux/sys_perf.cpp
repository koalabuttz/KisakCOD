#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>

#include "sys_perf.h"
#include "sys_perf_core.h"
#include "sys_posix.h"

#include <stdlib.h>
#include <time.h>

#if defined(__linux__)
#include "sys_perf_readings.h"
#endif

// ---------------------------------------------------------------------------
// The game's side of the A/B measuring in sys_perf_core.h: the commands, the
// clock, and where the counters and readings come from on this machine.
//
//   perf_ab "<commands A>" "<commands B>" [seconds] [rounds] [settle seconds]
//   perf_sample [seconds] [windows] [settle seconds]
//   perf_stop
//
// Everything is read from the main thread between frames: CPU time from the
// process and thread clocks, per-thread CPU time from /proc/self/task, and
// battery power, GPU and CPU frequency, package temperature and package energy
// from sysfs where this machine and this user can read them. A reading that is
// not there is left out of the lines; nothing is guessed.
// ---------------------------------------------------------------------------

namespace
{
    cmd_function_s s_perfAbCmd;
    cmd_function_s s_perfSampleCmd;
    cmd_function_s s_perfStopCmd;

    [[maybe_unused]] uint64_t ClockNs(clockid_t clock)
    {
        struct timespec ts;
        if (clock_gettime(clock, &ts) != 0)
            return 0;
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }

#if defined(__linux__)
    PerfReadingPaths s_paths;
#endif

    // WorkerCmdType, in the order of gfx_d3d/r_workercmds.h.
    const char *const s_workerTypeNames[] = {
        "fx spot light", "fx non dependent", "fx remaining", "dpvs cell static", "dpvs cell scene ent",
        "dpvs cell dyn model", "dpvs cell dyn brush", "dpvs entity", "add scene ent", "spot shadow ent",
        "shadow cookie", "bounds ent delayed", "skin ent delayed", "generate fx verts", "generate mark verts",
        "skin cached static model", "skin xmodel",
    };

    const char *LockName(int critSect)
    {
        switch (critSect)
        {
#define PERF_LOCK_NAME(name) case CRITSECT_##name: return #name;
        PERF_LOCK_NAME(CONSOLE) PERF_LOCK_NAME(DEBUG_SOCKET) PERF_LOCK_NAME(COM_ERROR) PERF_LOCK_NAME(STATMON)
        PERF_LOCK_NAME(DEBUG_LINE) PERF_LOCK_NAME(ALLOC_MARK) PERF_LOCK_NAME(SCRIPT_STRING)
        PERF_LOCK_NAME(MEMORY_TREE) PERF_LOCK_NAME(ASSERT) PERF_LOCK_NAME(SYS_EVENT_QUEUE)
        PERF_LOCK_NAME(GPU_FENCE) PERF_LOCK_NAME(FATAL_ERROR) PERF_LOCK_NAME(SCRIPT_DEBUGGER_ALLOC)
        PERF_LOCK_NAME(MISSING_ASSET) PERF_LOCK_NAME(PHYSICS) PERF_LOCK_NAME(LIVE) PERF_LOCK_NAME(AUDIO_PHYSICS)
        PERF_LOCK_NAME(CINEMATIC) PERF_LOCK_NAME(CINEMATIC_TARGET_CHANGE) PERF_LOCK_NAME(FX_ALLOC)
        PERF_LOCK_NAME(CBUF)
#if defined(KISAK_MP) && !defined(KISAK_RADIANT)
        PERF_LOCK_NAME(RD_BUFFER)
#else
        PERF_LOCK_NAME(SOUND_ALLOC) PERF_LOCK_NAME(MEM_ALLOC0) PERF_LOCK_NAME(MEM_ALLOC1)
        PERF_LOCK_NAME(STREAMED_SOUND) PERF_LOCK_NAME(FAKELAG) PERF_LOCK_NAME(CLIENT_MESSAGE)
        PERF_LOCK_NAME(CLIENT_CMD) PERF_LOCK_NAME(DOBJ_ALLOC) PERF_LOCK_NAME(START_SERVER)
        PERF_LOCK_NAME(XANIM_ALLOC) PERF_LOCK_NAME(KEY_BINDINGS) PERF_LOCK_NAME(FX_VIS)
        PERF_LOCK_NAME(SERVER_MESSAGE) PERF_LOCK_NAME(NETTHREAD_OVERRIDE)
#endif
#undef PERF_LOCK_NAME
        default: return nullptr;
        }
    }

    std::string NumberedName(const char *kind, const char *name, uint32_t index)
    {
        char text[96];
        if (name)
            snprintf(text, sizeof(text), "%s %s", kind, name);
        else
            snprintf(text, sizeof(text), "%s %u", kind, index);
        return text;
    }

    struct GameHost final : PerfHost
    {
        void Execute(const char *commandLine) override
        {
            Cbuf_AddText(0, commandLine);
            Cbuf_AddText(0, "\n");
        }

        void Print(const char *line) override
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "%s\n", line);
        }

        void ReadCounters(PerfCounters *counters) override
        {
            *counters = PerfCounters();
#if defined(CLOCK_THREAD_CPUTIME_ID) && defined(CLOCK_PROCESS_CPUTIME_ID) && !defined(__vita__)
            counters->mainCpuNs = ClockNs(CLOCK_THREAD_CPUTIME_ID);
            counters->processCpuNs = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
#endif
            counters->gpuWaitMainUs = g_sysPerfCounters[SYS_PERF_GPU_WAIT_MAIN].load(std::memory_order_relaxed);
            counters->gpuWaitBackendUs = g_sysPerfCounters[SYS_PERF_GPU_WAIT_BACKEND].load(std::memory_order_relaxed);
#if defined(__linux__)
            Perf_ReadPackageEnergy(s_paths, counters);
            Perf_ReadThreads("/proc/self/task", &counters->threads);
#endif
        }

        void ReadGauges(PerfGauges *gauges) override
        {
            *gauges = PerfGauges();
#if defined(__linux__)
            Perf_ReadGauges(s_paths, gauges);
#endif
        }

        size_t ProbeCount() override { return SYS_PERF_PROBE_COUNT; }

        std::string ProbeName(size_t probe) override
        {
            if (probe >= SYS_PERF_PROBE_LOCK)
            {
                const uint32_t lock = (uint32_t)(probe - SYS_PERF_PROBE_LOCK);
                return NumberedName("lock", LockName((int)lock), lock);
            }
            if (probe >= SYS_PERF_PROBE_WAIT_WORKER_TYPE)
            {
                const uint32_t type = (uint32_t)(probe - SYS_PERF_PROBE_WAIT_WORKER_TYPE);
                const size_t named = sizeof(s_workerTypeNames) / sizeof(s_workerTypeNames[0]);
                return NumberedName("wait for workers:", type < named ? s_workerTypeNames[type] : nullptr, type);
            }
            switch (probe)
            {
            case SYS_PERF_PROBE_SERVER_FRAME: return "server frame";
            case SYS_PERF_PROBE_HUD: return "hud";
            case SYS_PERF_PROBE_WAIT_RENDERER: return "wait for the render thread";
            case SYS_PERF_PROBE_WAIT_WORKERS_ALL: return "wait for workers: all";
            default: return "wait for workers: other";
            }
        }

        void ReadProbes(PerfProbeReading *readings) override
        {
            for (size_t i = 0; i < SYS_PERF_PROBE_COUNT; ++i)
            {
                readings[i].ns = g_sysPerfProbes[i].ns.load(std::memory_order_relaxed);
                readings[i].calls = g_sysPerfProbes[i].calls.load(std::memory_order_relaxed);
                readings[i].maxNs = g_sysPerfProbes[i].maxNs.exchange(0, std::memory_order_relaxed);
            }
        }

        void SetProbesActive(bool active) override
        {
            g_sysPerfProbesOn.store(active, std::memory_order_relaxed);
        }
    };

    GameHost s_host;
    PerfRunner s_runner(&s_host);

    double ArgSeconds(int index, double fallback)
    {
        if (Cmd_Argc() <= index)
            return fallback;
        const double value = atof(Cmd_Argv(index));
        return value > 0.0 ? value : fallback;
    }

    int ArgCount(int index, int fallback)
    {
        if (Cmd_Argc() <= index)
            return fallback;
        const int value = atoi(Cmd_Argv(index));
        return value > 0 ? value : fallback;
    }

    void StartRun(const PerfConfig &config)
    {
        if (s_runner.Active())
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "perf: a run is in progress; perf_stop ends it\n");
            return;
        }
#if defined(__linux__)
        Perf_FindReadingPaths("/", &s_paths);
#endif
        PerfConfig withTick = config;
        withTick.tickProbe = SYS_PERF_PROBE_SERVER_FRAME;
        s_runner.Start(withTick, Posix_MonotonicNanoseconds());
    }

    void PerfAb_f()
    {
        if (Cmd_Argc() < 3)
        {
            Com_Printf(CON_CHANNEL_SYSTEM,
                "usage: perf_ab \"<commands A>\" \"<commands B>\" [seconds 10] [rounds 4] [settle seconds 2]\n"
                "Measures frame time, CPU time and power in windows that alternate A B B A.\n");
            return;
        }
        PerfConfig config;
        config.variants.emplace_back(Cmd_Argv(1));
        config.variants.emplace_back(Cmd_Argv(2));
        config.sampleSeconds = ArgSeconds(3, 10.0);
        config.rounds = ArgCount(4, 4);
        config.settleSeconds = ArgSeconds(5, 2.0);
        StartRun(config);
    }

    void PerfSample_f()
    {
        PerfConfig config;
        config.variants.emplace_back();
        config.sampleSeconds = ArgSeconds(1, 10.0);
        config.rounds = ArgCount(2, 1);
        config.settleSeconds = ArgSeconds(3, 1.0);
        StartRun(config);
    }

    void PerfStop_f()
    {
        s_runner.Stop();
    }
}

void Sys_PerfInit()
{
    Cmd_AddCommandInternal("perf_ab", PerfAb_f, &s_perfAbCmd);
    Cmd_AddCommandInternal("perf_sample", PerfSample_f, &s_perfSampleCmd);
    Cmd_AddCommandInternal("perf_stop", PerfStop_f, &s_perfStopCmd);
}

void Sys_PerfFrame()
{
    if (s_runner.Active())
        s_runner.Frame(Posix_MonotonicNanoseconds());
}
