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
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    // Parts of the cells, of the world's surfaces and of the static models,
    // so that their time can be split. The first sits inside
    // SYS_PERF_PROBE_SCENE_CELLS, the next five inside
    // SYS_PERF_PROBE_SCENE_WORLD (a sun shadow's batches also copy indices),
    // the last two inside SYS_PERF_PROBE_SCENE_STATIC_MODELS. The two copies
    // are timed for every batch, hundreds a frame: take a clock reading off
    // each call.
    SYS_PERF_PROBE_CELLS_HAND_OVER,    // R_AddCellSurfacesAndCullGroupsInFrustumDelayed: a cell's four worker commands queued
    SYS_PERF_PROBE_WORLD_LIT,          // R_AddAllBspDrawSurfacesRangeCamera or ...CameraNonlit: the lit surfaces
    SYS_PERF_PROBE_WORLD_DECAL,        // ... the decals
    SYS_PERF_PROBE_WORLD_EMISSIVE,     // ... the emissive surfaces
    SYS_PERF_PROBE_WORLD_LIGHTS,       // the shadowed lights chosen, the draw method, the sun shadow's maps
    SYS_PERF_PROBE_WORLD_PRETESS,      // R_PreTessBspDrawSurfs: one batch's indices copied (inside the three first)
    SYS_PERF_PROBE_SMODEL_HAND_ON,     // R_SkinStaticModelsCameraForLod: a model's instances made draw surfaces
    SYS_PERF_PROBE_SMODEL_PRETESS,     // R_PreTessStaticModelCachedList: cached instances' indices copied (inside it)
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
    SYS_PERF_PROBE_RENDER_TESS_DRAWS = SYS_PERF_PROBE_RENDER_TESS + SYS_PERF_TESS_SLOTS,
                                       // + surface type: the same time again; its calls are the draws made
    SYS_PERF_PROBE_WAIT_RENDERER = SYS_PERF_PROBE_RENDER_TESS_DRAWS + SYS_PERF_TESS_SLOTS,
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

// `calls`: how many calls the time was for (a run of draws timed as one).
inline void Sys_PerfProbeAdd(uint32_t probe, uint64_t ns, uint32_t calls = 1)
{
    if (probe >= SYS_PERF_PROBE_COUNT)
        return;
    const uint32_t ns32 = ns > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ns;
    SysPerfProbeSlot &slot = g_sysPerfProbes[probe];
    slot.ns.fetch_add(ns32, std::memory_order_relaxed);
    slot.calls.fetch_add(calls, std::memory_order_relaxed);
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

// ---------------------------------------------------------------------------
// Phases of the render thread (perf_phases, docs/perf-ab.md): what the thread
// is in, read by a thread that looks about a thousand times a second.
//
// A probe above reads the clock twice, 1.4 us on the PS Vita, which is more
// than the work of most things a draw is made of. A phase scope writes a byte
// and counts its entry, and reads no clock, so it can sit around a call that
// happens thousands of times a frame; the share of the looks that find the
// thread in a phase, times the frame's length, over the entries in a frame,
// is what one call costs. The innermost scope is the one a look counts.
//
// The scopes sit in code that draws, which one thread at a time does: the
// render thread, or the main thread while it owns the device (a loading
// screen, a build without the render thread). Two threads inside scopes at
// once would only spoil the count.
//
// The scopes are compiled in Debug builds, which keeps them building on every
// platform, and in a build configured with -DKISAK_PERF_PHASES=ON, which is
// the one to measure with. Elsewhere a scope is nothing: fifteen thousand of
// them a frame would cost the Vita's render thread about 0.1 ms.
// ---------------------------------------------------------------------------
#if !defined(KISAK_PERF_PHASES)
#if defined(_DEBUG)
#define KISAK_PERF_PHASES 1
#else
#define KISAK_PERF_PHASES 0
#endif
#endif

enum SysPerfPhase : uint8_t
{
    SYS_PERF_PHASE_NONE,            // no scope: the thread's own loop, anything unmarked
    SYS_PERF_PHASE_IDLE,            // asleep until the main thread hands over a frame, or until it may present
    SYS_PERF_PHASE_PRESENT,         // RB_EndFrame: Present and what waits there
    SYS_PERF_PHASE_3D,              // RB_Draw3D, outside the lists below
    SYS_PERF_PHASE_2D,              // the frame's 2D commands, outside the text
    SYS_PERF_PHASE_TEXT_2D,         // DrawText2D: a string's letters made quads
    SYS_PERF_PHASE_TESS_END,        // R_DrawTessTechnique: a filled batch of 2D, effects or code meshes drawn
    SYS_PERF_PHASE_LIST,            // R_DrawSurfs: a draw list walked, between materials
    SYS_PERF_PHASE_MATERIAL,        // R_RenderDrawSurfListMaterial: R_SetupMaterial, a pass picked
    SYS_PERF_PHASE_PASS,            // R_SetupPass
    SYS_PERF_PHASE_STABLE_ARGS,     // R_SetPassShaderStableArguments: a pass's textures and constants
    SYS_PERF_PHASE_CRITICAL_ARGS,   // R_SetupPassCriticalPixelShaderArgs, R_SetupPassVertexShaderArgs
    SYS_PERF_PHASE_OBJECT_ARGS,     // R_SetupPassPerObjectArgs
    SYS_PERF_PHASE_PRIM_ARGS,       // R_SetupPassPerPrimArgs
    SYS_PERF_PHASE_MATRIX,          // R_GetCodeMatrix deriving a matrix
    SYS_PERF_PHASE_PLACEMENT,       // a model's world matrix made
    SYS_PERF_PHASE_LIGHTING_COORDS, // a model's lighting coordinates
    SYS_PERF_PHASE_STATE_BITS,      // R_ChangeState_0 and _1
    SYS_PERF_PHASE_VERTEX_DECL,     // R_UpdateVertexDecl
    SYS_PERF_PHASE_INDEX_FILL,      // R_SetIndexData: indices copied into the dynamic index buffer
    SYS_PERF_PHASE_VERTEX_FILL,     // R_SetVertexData: vertices copied into the dynamic vertex buffer
    SYS_PERF_PHASE_DEVICE_DRAW,     // the device: DrawIndexedPrimitive
    SYS_PERF_PHASE_DEVICE_VERTEX_CONSTANTS,
    SYS_PERF_PHASE_DEVICE_PIXEL_CONSTANTS,
    SYS_PERF_PHASE_DEVICE_TEXTURES, // textures and sampler states
    SYS_PERF_PHASE_DEVICE_STREAMS,  // vertex streams, the index buffer, the vertex declaration
    SYS_PERF_PHASE_DEVICE_SHADERS,  // the vertex and pixel shader set
    SYS_PERF_PHASE_TESS,            // + surface type: rb_tessTable's function, outside all of the above
    SYS_PERF_PHASE_COUNT = SYS_PERF_PHASE_TESS + SYS_PERF_TESS_SLOTS
};

// The name a phase prints under. `text` is used for the surface types.
inline const char *Sys_PerfPhaseName(uint32_t phase, char *text, size_t textSize)
{
    static const char *const kTess[SYS_PERF_TESS_SLOTS] = {
        "triangles", "pretessellated triangles", "static models", "pretessellated static models",
        "cached static models", "skinned static models", "brush models", "rigid models",
        "rigid skinned models", "skinned models", "code meshes", "marks", "particle clouds"};
    if (phase >= SYS_PERF_PHASE_TESS && phase < SYS_PERF_PHASE_COUNT)
    {
        snprintf(text, textSize, "tess %s", kTess[phase - SYS_PERF_PHASE_TESS]);
        return text;
    }
    switch (phase)
    {
    case SYS_PERF_PHASE_NONE: return "unmarked";
    case SYS_PERF_PHASE_IDLE: return "waiting for a frame";
    case SYS_PERF_PHASE_PRESENT: return "present";
    case SYS_PERF_PHASE_3D: return "3d outside the lists";
    case SYS_PERF_PHASE_2D: return "2d";
    case SYS_PERF_PHASE_TEXT_2D: return "2d text";
    case SYS_PERF_PHASE_TESS_END: return "a filled batch drawn";
    case SYS_PERF_PHASE_LIST: return "list walk";
    case SYS_PERF_PHASE_MATERIAL: return "material setup";
    case SYS_PERF_PHASE_PASS: return "pass setup";
    case SYS_PERF_PHASE_STABLE_ARGS: return "a pass's textures and constants";
    case SYS_PERF_PHASE_CRITICAL_ARGS: return "critical args";
    case SYS_PERF_PHASE_OBJECT_ARGS: return "per-object args";
    case SYS_PERF_PHASE_PRIM_ARGS: return "per-prim args";
    case SYS_PERF_PHASE_MATRIX: return "matrices derived";
    case SYS_PERF_PHASE_PLACEMENT: return "model placement";
    case SYS_PERF_PHASE_LIGHTING_COORDS: return "lighting coords";
    case SYS_PERF_PHASE_STATE_BITS: return "state bits";
    case SYS_PERF_PHASE_VERTEX_DECL: return "vertex declaration";
    case SYS_PERF_PHASE_INDEX_FILL: return "index fill";
    case SYS_PERF_PHASE_VERTEX_FILL: return "vertex fill";
    case SYS_PERF_PHASE_DEVICE_DRAW: return "device: draw";
    case SYS_PERF_PHASE_DEVICE_VERTEX_CONSTANTS: return "device: vertex constants";
    case SYS_PERF_PHASE_DEVICE_PIXEL_CONSTANTS: return "device: pixel constants";
    case SYS_PERF_PHASE_DEVICE_TEXTURES: return "device: textures";
    case SYS_PERF_PHASE_DEVICE_STREAMS: return "device: streams";
    case SYS_PERF_PHASE_DEVICE_SHADERS: return "device: shaders";
    default: return "?";
    }
}

#if KISAK_PERF_PHASES
// Written by the render thread only, read by the thread that looks. Relaxed
// atomics: a plain load and a plain store on every platform here, and a count
// that is one entry behind is as good.
inline std::atomic<uint8_t> g_sysPerfPhase;
inline std::atomic<uint32_t> g_sysPerfPhaseEntries[SYS_PERF_PHASE_COUNT]; // wraps; the reader takes differences

class SysPerfPhaseScope
{
public:
    explicit SysPerfPhaseScope(uint8_t phase) : before_(g_sysPerfPhase.load(std::memory_order_relaxed))
    {
        g_sysPerfPhase.store(phase, std::memory_order_relaxed);
        std::atomic<uint32_t> &entries = g_sysPerfPhaseEntries[phase];
        entries.store(entries.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    ~SysPerfPhaseScope() { g_sysPerfPhase.store(before_, std::memory_order_relaxed); }
    SysPerfPhaseScope(const SysPerfPhaseScope &) = delete;
    SysPerfPhaseScope &operator=(const SysPerfPhaseScope &) = delete;

private:
    uint8_t before_;
};
#else
class SysPerfPhaseScope
{
public:
    explicit SysPerfPhaseScope(uint8_t) {}
    ~SysPerfPhaseScope() {} // not trivial: a scope with no other use is not an unused variable
    SysPerfPhaseScope(const SysPerfPhaseScope &) = delete;
    SysPerfPhaseScope &operator=(const SysPerfPhaseScope &) = delete;
};
#endif
