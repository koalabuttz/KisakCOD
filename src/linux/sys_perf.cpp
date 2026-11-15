#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>

#include "sys_perf.h"
#include "sys_perf_core.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <qcommon/threads.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include "sys_posix.h"
#include <time.h>
#endif

#if defined(__linux__)
#include "sys_perf_readings.h"
#endif

#if defined(__vita__)
#include <vita/vita_platform.h> // Vita_ThreadTimes
#endif

// ---------------------------------------------------------------------------
// The game's side of the A/B measuring in sys_perf_core.h: the commands, the
// clock, and where the counters and readings come from on this machine.
//
//   perf_ab "<commands A>" "<commands B>" [seconds] [rounds] [settle seconds] [timed rows]
//   perf_sample [seconds] [windows] [settle seconds] [timed rows]
//   perf_stop
//
// Everything is read from the main thread between frames: CPU time from the
// process and thread clocks, per-thread CPU time from /proc/self/task, and
// battery power, GPU and CPU frequency, package temperature and package energy
// from sysfs where this machine and this user can read them. A reading that is
// not there is left out of the lines; nothing is guessed.
//
// On Windows the CPU times come from GetThreadTimes and GetProcessTimes, which
// count in whole scheduler ticks (about 15.6 ms), so use windows of 10 s or
// more; the threads are listed with a Toolhelp snapshot and named by the
// engine's thread table ("other" for threads the engine did not start, such
// as the driver's). There are no machine readings.
// ---------------------------------------------------------------------------

namespace
{
    cmd_function_s s_perfAbCmd;
    cmd_function_s s_perfSampleCmd;
    cmd_function_s s_perfStopCmd;

#if defined(_WIN32)
    uint64_t NowNs()
    {
        return Sys_PerfNowNs();
    }

    uint64_t FileTimeNs(const FILETIME &time)
    {
        return ((uint64_t)time.dwHighDateTime << 32 | time.dwLowDateTime) * 100ull;
    }

    // Kernel plus user time of a thread, or 0 when it cannot be read.
    uint64_t ThreadCpuNs(HANDLE thread)
    {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(thread, &created, &exited, &kernel, &user))
            return 0;
        return FileTimeNs(kernel) + FileTimeNs(user);
    }

    uint64_t ProcessCpuNs()
    {
        FILETIME created, exited, kernel, user;
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return 0;
        return FileTimeNs(kernel) + FileTimeNs(user);
    }

    const char *EngineThreadName(DWORD id)
    {
        for (int context = 0; context < THREAD_CONTEXT_COUNT; ++context)
        {
            if (threadId[context] == id)
                return Sys_GetThreadContextName(context);
        }
        return nullptr;
    }

    // Every thread of this process with its CPU time, as Perf_ReadThreads
    // does from /proc on Linux.
    void ReadThreads(std::vector<PerfThreadTime> *threads)
    {
        threads->clear();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        const DWORD process = GetCurrentProcessId();
        THREADENTRY32 entry;
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != process)
                continue;
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            PerfThreadTime time;
            time.tid = (int)entry.th32ThreadID;
            time.runNs = ThreadCpuNs(thread);
            CloseHandle(thread);
            const char *name = EngineThreadName(entry.th32ThreadID);
            time.name = name ? name : "other";
            threads->push_back(time);
        }
        CloseHandle(snapshot);
    }
#else
    uint64_t NowNs()
    {
        return Posix_MonotonicNanoseconds();
    }

    [[maybe_unused]] uint64_t ClockNs(clockid_t clock)
    {
        struct timespec ts;
        if (clock_gettime(clock, &ts) != 0)
            return 0;
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
#endif

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
#if defined(_WIN32)
            // Called on the main thread.
            counters->mainCpuNs = ThreadCpuNs(GetCurrentThread());
            counters->processCpuNs = ProcessCpuNs();
            ReadThreads(&counters->threads);
#elif defined(__vita__)
            // Called on the main thread. newlib has no CPU time clocks; the
            // kernel counts each thread's run time, and "all" is the sum over
            // the threads the port started (vita_threads.cpp), not the
            // system's own threads in this process.
            {
                VitaThreadTime times[24];
                const int count = Vita_ThreadTimes(times, 24);
                for (int i = 0; i < count; ++i)
                {
                    PerfThreadTime time;
                    time.tid = times[i].id;
                    time.name = times[i].name;
                    time.runNs = times[i].runMicroseconds * 1000ull;
                    counters->threads.push_back(time);
                    counters->processCpuNs += time.runNs;
                    if (times[i].self)
                        counters->mainCpuNs = time.runNs;
                }
            }
#elif defined(CLOCK_THREAD_CPUTIME_ID) && defined(CLOCK_PROCESS_CPUTIME_ID)
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
            if (probe >= SYS_PERF_PROBE_RENDER_TESS && probe < SYS_PERF_PROBE_RENDER_TESS_DRAWS + SYS_PERF_TESS_SLOTS)
            {
                static const char *const kTess[SYS_PERF_TESS_SLOTS] = {
                    "triangles", "pretessellated triangles", "static models", "pretessellated static models",
                    "cached static models", "skinned static models", "brush models", "rigid models",
                    "rigid skinned models", "skinned models", "code meshes", "marks", "particle clouds"};
                if (probe >= SYS_PERF_PROBE_RENDER_TESS_DRAWS)
                    return std::string("render: draws by tess ") + kTess[probe - SYS_PERF_PROBE_RENDER_TESS_DRAWS];
                return std::string("render: tess ") + kTess[probe - SYS_PERF_PROBE_RENDER_TESS];
            }
            if (probe >= SYS_PERF_PROBE_WORK_MAIN)
            {
                const bool main = probe < SYS_PERF_PROBE_WORK_OTHER;
                const uint32_t type =
                    (uint32_t)(probe - (main ? SYS_PERF_PROBE_WORK_MAIN : SYS_PERF_PROBE_WORK_OTHER));
                const size_t named = sizeof(s_workerTypeNames) / sizeof(s_workerTypeNames[0]);
                return NumberedName(main ? "main thread runs:" : "other threads run:",
                                    type < named ? s_workerTypeNames[type] : nullptr, type);
            }
            if (probe == SYS_PERF_PROBE_WORKERS_AWAKE)
                return "workers: awake";
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
            case SYS_PERF_PROBE_SERVER_BOTS: return "server: bot moves";
            case SYS_PERF_PROBE_SERVER_TRIGGERS: return "server: triggers";
            case SYS_PERF_PROBE_SERVER_ANIM: return "server: animation";
            case SYS_PERF_PROBE_SERVER_SCRIPTS: return "server: scripts";
            case SYS_PERF_PROBE_SERVER_ENTITIES: return "server: entities";
            case SYS_PERF_PROBE_SERVER_CLIENT_END: return "server: player end frame";
            case SYS_PERF_PROBE_SERVER_SEND: return "server: snapshots";
            case SYS_PERF_PROBE_HUD_MENUS: return "hud: menus";
            case SYS_PERF_PROBE_HUD_ELEMS: return "hud: script elements";
            case SYS_PERF_PROBE_HUD_NAMES: return "hud: crosshair and names";
            case SYS_PERF_PROBE_HUD_COMPASS: return "hud: compass";
            case SYS_PERF_PROBE_HUD_SCOREBOARD: return "hud: scoreboard";
            case SYS_PERF_PROBE_UI_EXPRESSIONS: return "menu expressions";
            case SYS_PERF_PROBE_CLIENT_FRAME: return "client: frame";
            case SYS_PERF_PROBE_CLIENT_SCREEN: return "client: screen";
            case SYS_PERF_PROBE_CLIENT_SNAPSHOTS: return "client: snapshots";
            case SYS_PERF_PROBE_CLIENT_ENTITIES: return "client: entities";
            case SYS_PERF_PROBE_CLIENT_PREDICT: return "client: input and prediction";
            case SYS_PERF_PROBE_CLIENT_VIEW: return "client: view and weapon";
            case SYS_PERF_PROBE_CLIENT_SCENE: return "client: scene";
            case SYS_PERF_PROBE_CLIENT_SOUND: return "client: sound";
            case SYS_PERF_PROBE_CLIENT_2D: return "client: 2d";
            case SYS_PERF_PROBE_CLIENT_HAND_OVER: return "client: hand over the frame";
            case SYS_PERF_PROBE_SCENE_CELLS: return "scene: cells";
            case SYS_PERF_PROBE_SCENE_WORLD: return "scene: world surfaces";
            case SYS_PERF_PROBE_SCENE_STATIC_MODELS: return "scene: static models";
            case SYS_PERF_PROBE_SCENE_ENTITIES: return "scene: entities";
            case SYS_PERF_PROBE_SCENE_MERGE: return "scene: sorted lists";
            case SYS_PERF_PROBE_RENDER_3D: return "render: 3d";
            case SYS_PERF_PROBE_RENDER_DEPTH: return "render: depth pass";
            case SYS_PERF_PROBE_RENDER_LIT: return "render: lit";
            case SYS_PERF_PROBE_RENDER_DECAL: return "render: decals";
            case SYS_PERF_PROBE_RENDER_SUN: return "render: sun";
            case SYS_PERF_PROBE_RENDER_LIGHTS: return "render: point lights";
            case SYS_PERF_PROBE_RENDER_EMISSIVE: return "render: emissive";
            case SYS_PERF_PROBE_RENDER_POSTFX: return "render: post effects";
            case SYS_PERF_PROBE_RENDER_2D: return "render: 2d";
            case SYS_PERF_PROBE_RENDER_SWAP_WAIT: return "render: wait before present";
            case SYS_PERF_PROBE_RENDER_PRESENT: return "render: present";
            case SYS_PERF_PROBE_RENDER_IDLE: return "render: waiting for a frame";
            case SYS_PERF_PROBE_RENDER_MATERIAL: return "render: material setup";
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

    GameHost s_gameHost;
    PerfRunner s_runner(&s_gameHost);

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
        s_runner.Start(withTick, NowNs());
    }

    void PerfAb_f()
    {
        if (Cmd_Argc() < 3)
        {
            Com_Printf(CON_CHANNEL_SYSTEM,
                "usage: perf_ab \"<commands A>\" \"<commands B>\" [seconds 10] [rounds 4] [settle seconds 2]"
                " [timed rows 16]\n"
                "Measures frame time, CPU time and power in windows that alternate A B B A.\n");
            return;
        }
        PerfConfig config;
        config.variants.emplace_back(Cmd_Argv(1));
        config.variants.emplace_back(Cmd_Argv(2));
        config.sampleSeconds = ArgSeconds(3, 10.0);
        config.rounds = ArgCount(4, 4);
        config.settleSeconds = ArgSeconds(5, 2.0);
        config.probeRows = (size_t)ArgCount(6, (int)config.probeRows);
        StartRun(config);
    }

    void PerfSample_f()
    {
        PerfConfig config;
        config.variants.emplace_back();
        config.sampleSeconds = ArgSeconds(1, 10.0);
        config.rounds = ArgCount(2, 1);
        config.settleSeconds = ArgSeconds(3, 1.0);
        config.probeRows = (size_t)ArgCount(4, (int)config.probeRows);
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
        s_runner.Frame(NowNs());
}
