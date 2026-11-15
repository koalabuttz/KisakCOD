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
inline constexpr uint32_t SYS_PERF_TESS_SLOTS = 13;        // rb_tessTable's surface types

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
    // Parts of the HUD (cgame_mp), inside SYS_PERF_PROBE_HUD.
    SYS_PERF_PROBE_HUD_MENUS,          // Menu_PaintAll: the HUD's menus
    SYS_PERF_PROBE_HUD_ELEMS,          // CG_Draw2dHudElems: script hud elements
    SYS_PERF_PROBE_HUD_NAMES,          // CG_ScanForCrosshairEntity and CG_UpdatePlayerNames
    SYS_PERF_PROBE_HUD_COMPASS,        // CG_CompassUpdateActors
    SYS_PERF_PROBE_HUD_SCOREBOARD,     // CG_DrawScoreboard
    SYS_PERF_PROBE_UI_EXPRESSIONS,     // EvaluateExpression: every menu expression, in the HUD or not
    // Parts of painting a menu (ui), in the HUD or not, so the time outside the
    // expressions can be split. The item parts sit inside Item_Paint.
    SYS_PERF_PROBE_UI_WINDOWS,         // Window_Paint: a menu's or an item's background and border
    SYS_PERF_PROBE_UI_TEXT_ITEMS,      // Item_Text_Paint, with its translation and text expression
    SYS_PERF_PROBE_UI_OWNERDRAW_ITEMS, // Item_OwnerDraw_Paint: items the game draws itself (ammo, compass, ...)
    SYS_PERF_PROBE_UI_OTHER_ITEMS,     // every other item type's paint
    SYS_PERF_PROBE_UI_TRANSLATE,       // UI_SafeTranslateString: a "@NAME" text looked up
    SYS_PERF_PROBE_UI_MATERIAL_NAMES,  // Item_Paint: a background material found by its name each frame
    SYS_PERF_PROBE_UI_SHOW_BY_DVAR,    // Item_EnableShowViaDvar: an item shown or hidden by a dvar's value
    // The main thread's client frame (client_mp, cgame_mp), so its time can
    // be split. All but the first sit inside SYS_PERF_PROBE_CLIENT_SCREEN;
    // the HUD's probes above do too.
    SYS_PERF_PROBE_CLIENT_FRAME,       // CL_Frame: packets in and out, the client's own upkeep
    SYS_PERF_PROBE_CLIENT_SCREEN,      // SCR_UpdateScreen: everything a frame draws, the scene's making included
    SYS_PERF_PROBE_CLIENT_SNAPSHOTS,   // CG_ProcessSnapshots
    SYS_PERF_PROBE_CLIENT_ENTITIES,    // CG_AddPacketEntities: every entity of the snapshot
    SYS_PERF_PROBE_CLIENT_PREDICT,     // CL_Input and CG_PredictPlayerState
    SYS_PERF_PROBE_CLIENT_VIEW,        // the view's values, the view weapon, the player's own entity
    SYS_PERF_PROBE_CLIENT_SCENE,       // CG_DrawActive: R_RenderScene, with its waits for workers
    SYS_PERF_PROBE_CLIENT_SOUND,       // CL_UpdateSound (inside the scene in a map)
    SYS_PERF_PROBE_CLIENT_2D,          // SCR_DrawScreenField and CL_DrawScreen: menus over the game, the console
    SYS_PERF_PROBE_CLIENT_HAND_OVER,   // R_EndFrame and R_IssueRenderCommands, with the wait for the render thread
    // Parts of R_GenerateSortedDrawSurfs on the main thread (gfx_d3d), inside
    // SYS_PERF_PROBE_CLIENT_SCENE.
    SYS_PERF_PROBE_SCENE_CELLS,        // R_AddWorldSurfacesDpvs: the visible cells, handed to the workers
    SYS_PERF_PROBE_SCENE_WORLD,        // the world's surfaces added, and the sun shadow's maps set up
    SYS_PERF_PROBE_SCENE_STATIC_MODELS, // R_AddAllStaticModelSurfacesCamera
    SYS_PERF_PROBE_SCENE_ENTITIES,     // R_DrawAllSceneEnt
    SYS_PERF_PROBE_SCENE_MERGE,        // R_MergeAndEmitDrawSurfLists and R_SortDrawSurfs: the lists sorted
    // The render thread (gfx_d3d), so its frame can be split as the main
    // thread's is. The 3D parts sit inside SYS_PERF_PROBE_RENDER_3D.
    SYS_PERF_PROBE_RENDER_3D,          // RB_Draw3D: the 3D views of a frame
    SYS_PERF_PROBE_RENDER_DEPTH,       // R_DepthPrepass: the scene's depth drawn first
    SYS_PERF_PROBE_RENDER_LIT,         // R_DrawLit: opaque lit surfaces
    SYS_PERF_PROBE_RENDER_DECAL,       // R_DrawDecal
    SYS_PERF_PROBE_RENDER_SUN,         // RB_DrawSun
    SYS_PERF_PROBE_RENDER_LIGHTS,      // R_DrawLights: surfaces lit by point lights
    SYS_PERF_PROBE_RENDER_EMISSIVE,    // R_DrawEmissive: effects, blended surfaces
    SYS_PERF_PROBE_RENDER_POSTFX,      // RB_EndSceneRendering: post effects of a view
    SYS_PERF_PROBE_RENDER_2D,          // RB_CallExecuteRenderCommands: the HUD, menus, 2D
    SYS_PERF_PROBE_RENDER_SWAP_WAIT,   // the render thread's wait before Present
    SYS_PERF_PROBE_RENDER_PRESENT,     // RB_EndFrame: Present
    SYS_PERF_PROBE_RENDER_IDLE,        // the render thread asleep until the main thread hands it a frame
    // Inside the 3D parts' R_DrawSurfs: a run of surfaces of one material.
    SYS_PERF_PROBE_RENDER_MATERIAL,    // R_SetupMaterial and R_SetupPass: the material's state and constants
    SYS_PERF_PROBE_RENDER_TESS,        // + surface type: rb_tessTable's function, with its draws
    SYS_PERF_PROBE_WAIT_RENDERER = SYS_PERF_PROBE_RENDER_TESS + SYS_PERF_TESS_SLOTS,
                                       // main thread asleep until the render thread takes the frame
    SYS_PERF_PROBE_WAIT_WORKERS_ALL,   // ... until every front-end worker command is done
    SYS_PERF_PROBE_WAIT_WORKERS_OTHER, // ... in any other wait that runs worker commands
    SYS_PERF_PROBE_WAIT_WORKER_TYPE,   // + WorkerCmdType: ... until the commands of that type are done
    // Worker commands as they run (gfx_d3d/r_workercmds.cpp). The main thread
    // runs them too while it waits, so its share is inside its wait above.
    SYS_PERF_PROBE_WORKERS_AWAKE = SYS_PERF_PROBE_WAIT_WORKER_TYPE + SYS_PERF_WORKER_TYPE_SLOTS,
                                       // a worker thread from waking to its next sleep; calls are wake-ups
    SYS_PERF_PROBE_WORK_MAIN,          // + WorkerCmdType: commands of that type run by the main thread
    SYS_PERF_PROBE_WORK_OTHER = SYS_PERF_PROBE_WORK_MAIN + SYS_PERF_WORKER_TYPE_SLOTS,
                                       // + WorkerCmdType: ... run by any other thread
    SYS_PERF_PROBE_LOCK = SYS_PERF_PROBE_WORK_OTHER + SYS_PERF_WORKER_TYPE_SLOTS,
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
