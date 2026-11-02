// World draw probe: see world_draw_probe.h and docs/linux-world-draw-probe.md.
//
// Every device object and every draw comes from the engine's own code: R_Init
// starts the renderer, the zone loader creates the textures, shaders and
// buffers, R_LoadWorld registers the map, and the scene and backend units draw
// it. This file only starts those, chooses the camera, reads the back buffer
// back into memory and measures it. It saves no image.

#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_bsp.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <database/database.h>
#include <client_mp/client_mp.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/rb_logfile.h>
#include <gfx_d3d/rb_stats.h>
#include <linux/d3d9_types.h>
#include <linux/client/window/sdl_api.h>

#include "world_draw_engine.h"
#include "world_draw_probe.h"
#include "world_draw_rules.h"
#include "world_frontend_invariants.h"
#include "world_surface_export.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// The zone table (src/database/db_registry.cpp). Read only.
extern int32_t g_zoneCount;

namespace
{
namespace wf = kisak_world_frontend;
namespace wdr = kisak_world_draw;
using namespace kisak_world_draw_engine;

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 480;
// cg_fov's default, 65 degrees across a 4:3 view, as CG_CalcFov turns it into
// the refdef's tangents.
constexpr float kHalfFovXDegrees = 32.5f;
// The scene time every frame is drawn at. R_GenerateReflectionRawData draws at
// 0 too; one fixed time makes a repeated draw of a view comparable.
constexpr int kSceneTime = 0;
// Each view is drawn until it settles (wdr::CheckSettled: two draws in a row with
// the same pixels, the second moving nothing new into the static model cache), at
// most this many times. The first draws of a view differ while the renderer's
// per-frame state settles: the static model cache takes at most 256 surfaces and
// 8192 vertices a frame (R_CacheStaticModelSurface), for the camera and the sun
// shadow map, so a view's models move into it over several frames. The GPU runs
// of mp_crash are in docs/linux-world-draw-probe.md.
constexpr int kMaxSettleDraws = 64;
constexpr int kMaxShowSeconds = 60;

void SetBool(const dvar_s *dvar, bool value)
{
    Dvar_SetBool(const_cast<dvar_s *>(dvar), value);
}

// ---- the environment ----------------------------------------------------------

// DXVK reads DXVK_WSI_DRIVER when the first device is made. The windows here are
// SDL3 windows, so any other WSI would misread every HWND.
void SelectWsi()
{
    const char *const current = std::getenv("DXVK_WSI_DRIVER");
    if (!current)
    {
        Require(setenv("DXVK_WSI_DRIVER", "SDL3", 1) == 0, "environment", "cannot set DXVK_WSI_DRIVER=SDL3");
        Say("dxvk_wsi_driver=SDL3 (set by the probe)\n");
        return;
    }
    Require(!std::strcmp(current, "SDL3"), "environment",
            std::string("DXVK_WSI_DRIVER is '") + current +
                "'; the engine's windows are SDL3 windows, so it must be SDL3 or unset");
    Say("dxvk_wsi_driver=SDL3\n");
}

// DXVK Native writes <exe>_d3d9.log into the working directory unless
// DXVK_LOG_PATH says otherwise. The probe writes nothing outside its private
// output directory, so the file is off unless the caller chose a path.
void KeepDxvkLogOnStderr()
{
    const char *const current = std::getenv("DXVK_LOG_PATH");
    if (current)
    {
        Say("dxvk_log_path=%s\n", current);
        return;
    }
    Require(setenv("DXVK_LOG_PATH", "none", 1) == 0, "environment", "cannot set DXVK_LOG_PATH=none");
    Say("dxvk_log_path=none (set by the probe; DXVK logs to stderr)\n");
}

// DXVK's SDL3 WSI starts SDL video inside Direct3DCreate9 and throws when it
// cannot; that exception does not unwind out of libdxvk_d3d9.so. Starting video
// here first makes a missing display this stage's failure.
void StartVideo()
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        Fail("environment", std::string("SDL video did not start (no display?): ") + SDL_GetError());
    const char *const driver = SDL_GetCurrentVideoDriver();
    Say("video_driver=%s\n", driver ? driver : "none");
}

// R_InitGraphicsApi ignores R_PreCreateWindow's answer and goes on to create a
// device on whatever interface it got, so a missing D3D9 library or Vulkan
// driver would crash inside R_Init. The probe asks for an interface once first,
// releases it, and names this stage instead.
void RequireDirect3D()
{
    IDirect3D9 *const d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    Require(d3d9 != nullptr, "direct3d",
            "Direct3DCreate9 returned no interface (no DXVK library, no Vulkan driver, or no adapter)");
    d3d9->Release();
    Say("direct3d=available\n");
}

// ---- the frame ------------------------------------------------------------

// One frame, call for call as R_GenerateReflectionRawData draws a cubemap face,
// without the effects update (no effects system runs here) and with the view
// values the cgame's CG_CalcViewValues would give a player standing at the
// camera: the whole scene viewport, cg_fov's default, zNear 0.
FrameResult DrawView(const Camera &camera, refdef_s &refdef)
{
    PrepareRefdef(refdef);
    refdef.time = kSceneTime;
    refdef.x = 0;
    refdef.y = 0;
    refdef.width = vidConfig.sceneWidth;
    refdef.height = vidConfig.sceneHeight;
    refdef.tanHalfFovX = std::tan(kHalfFovXDegrees * float(M_PI) / 180.0f);
    refdef.tanHalfFovY = refdef.tanHalfFovX * float(refdef.height) / float(refdef.width);
    refdef.zNear = 0.0f;
    for (int i = 0; i < 3; ++i)
        refdef.vieworg[i] = camera.eye[i];
    AnglesToAxis(camera.angles, refdef.viewaxis);
    return DrawRefdef(refdef);
}

std::chrono::steady_clock::time_point g_started;
// KISAK_WORLD_DRAW_SHOW_SECONDS, read with the other arguments before any device work.
int g_showSeconds = 0;
// KISAK_FREE_CAMERA_INPUT and KISAK_FREE_CAMERA_SECONDS, read the same way.
FreeCameraInput g_cameraInput = FreeCameraInput::Replay;
int g_cameraSeconds = 0;
constexpr int kMaxCameraSeconds = 3600;
} // namespace

// The client's renderer configuration (src/client_mp/cl_main_mp.cpp CL_InitRef:
// SetupGfxConfig, CL_SetFastFileNames, R_ConfigureRenderer) with the dedicated
// server's zone set (no ui_mp), for a renderer that runs on this thread alone. Unlike the dedicated server's
// CL_InitDedicated (src/linux/server_device_counterparts.cpp) it latches nothing
// off and loads no zone: Kisak_StartWorldDraw starts the renderer.
void __cdecl CL_InitDedicated()
{
    if (!onlinegame)
        onlinegame = Dvar_RegisterBool("onlinegame", 1, DVAR_CHEAT,
                                       "Current game is an online game with stats, custom classes, unlocks");
    // One thread: with sys_smp_allowed off, R_RegisterDvars makes the worker
    // thread dvars default off and read-only, R_AddWorkerCmd runs each worker
    // command in place, and R_IssueRenderCommands runs the backend here. Set
    // before the renderer dvars exist, as a command-line +set would be.
    Dvar_SetBool(const_cast<dvar_s *>(sys_smp_allowed), false);
    GfxConfiguration config{};
    config.maxClientViews = 1;
    config.entCount = MAX_GENTITIES;
    config.entnumNone = ENTITYNUM_NONE;
    config.entnumOrdinaryEnd = ENTITYNUM_WORLD;
    config.threadContextCount = THREAD_CONTEXT_COUNT;
    config.critSectCount = CRITSECT_COUNT;
    config.codeFastFileName = "code_post_gfx_mp";
    config.uiFastFileName = nullptr;
    config.commonFastFileName = "common_mp";
    config.localizedCodeFastFileName = "localized_code_post_gfx_mp";
    config.localizedCommonFastFileName = "localized_common_mp";
    config.modFastFileName = DB_ModFileExists() ? "mod" : nullptr;
    // As the client's CL_InitRef: SetGfxConfig, then R_InitRenderCommands, which
    // sizes the model lighting atlas and allocates the render command buffers
    // and scene buffers that R_Init and every frame use.
    R_ConfigureRenderer(&config);
    if (!r_loadForRenderer)
        R_RegisterDvars();
    // The effects dvars the scene reads every frame. No effects system starts,
    // so no mark exists: fx_marks off is what a player with marks disabled has.
    FX_RegisterDvars();
    Dvar_SetBool(const_cast<dvar_s *>(fx_marks), false);
    Sys_HideSplashWindow();
    Sys_ShowConsole();
    Sys_NormalExit();
}

void Kisak_StartWorldDraw(WorldDrawMode drawMode, const char *mapName, const wf::Layout &loaderLayout)
{
    g_started = std::chrono::steady_clock::now();
    const bool freeCamera = drawMode == WorldDrawMode::FreeCamera;
    const char *const input = std::getenv("KISAK_FREE_CAMERA_INPUT");
    const char *const cameraSeconds = std::getenv("KISAK_FREE_CAMERA_SECONDS");
    if (freeCamera && input && !std::strcmp(input, "window"))
        g_cameraInput = FreeCameraInput::Window;
    else if (freeCamera && input && !std::strcmp(input, "script"))
        g_cameraInput = FreeCameraInput::Script;
    if (freeCamera)
        SetTool(g_cameraInput == FreeCameraInput::Replay ? "free_camera_replay" : "free_camera_window");
    Require(!input || freeCamera, "arguments", "KISAK_FREE_CAMERA_INPUT belongs to KISAK_FREE_CAMERA_MAP");
    Require(!input || g_cameraInput != FreeCameraInput::Replay, "arguments",
            "KISAK_FREE_CAMERA_INPUT must be window or script");
    Require(!cameraSeconds || g_cameraInput == FreeCameraInput::Window, "arguments",
            "KISAK_FREE_CAMERA_SECONDS belongs to KISAK_FREE_CAMERA_INPUT=window");
    if (cameraSeconds)
    {
        char *end = nullptr;
        const long value = std::strtol(cameraSeconds, &end, 10);
        Require(end != cameraSeconds && *end == '\0' && value >= 1 && value <= kMaxCameraSeconds, "arguments",
                "KISAK_FREE_CAMERA_SECONDS must be a whole number from 1 to " + std::to_string(kMaxCameraSeconds));
        g_cameraSeconds = int(value);
    }
    const char *const mapVariable = freeCamera ? "KISAK_FREE_CAMERA_MAP" : "KISAK_WORLD_DRAW_MAP";
    Require(mapName && kisak_world_inventory::IsAllowedMapName(mapName), "arguments",
            std::string(mapVariable) + " must name an allowlisted map");
    const char *const show = std::getenv("KISAK_WORLD_DRAW_SHOW_SECONDS");
    Require(!show || !freeCamera, "arguments",
            "KISAK_WORLD_DRAW_SHOW_SECONDS belongs to KISAK_WORLD_DRAW_MAP; the free-camera replay shows every "
            "frame it draws");
    if (show)
    {
        char *end = nullptr;
        const long value = std::strtol(show, &end, 10);
        Require(end != show && *end == '\0' && value >= 0 && value <= kMaxShowSeconds, "arguments",
                "KISAK_WORLD_DRAW_SHOW_SECONDS must be a whole number from 0 to " + std::to_string(kMaxShowSeconds));
        g_showSeconds = int(value);
    }
    // The loader and this probe are compiled with different definitions, and this
    // side sees the D3D9 types through DXVK's headers: both must agree on every
    // struct they hand each other before anything is created.
    const wf::Layout probeLayout = KISAK_WORLD_FRONTEND_LAYOUT();
    const size_t differs = wf::FirstLayoutDifference(loaderLayout, probeLayout);
    Require(differs == wf::kLayoutTypeCount, "layout",
            differs < wf::kLayoutTypeCount
                ? std::string("struct ") + wf::kLayoutTypeNames[differs] + " is " +
                      std::to_string(loaderLayout.size[differs]) + " bytes to the loader and " +
                      std::to_string(probeLayout.size[differs]) + " to the probe"
                : std::string());
    Say("world_draw_layout=agreed types=%zu\n", wf::kLayoutTypeCount);

    // Nothing may exist yet, and the configuration CL_InitDedicated made must hold.
    Require(g_zoneCount == 0, "start", "a zone was loaded before the renderer started");
    Require(dx.d3d9 == nullptr && dx.device == nullptr, "start", "a D3D object exists before the renderer started");
    Require(rgp.world == nullptr && !rg.registered, "start", "the renderer was registered before the probe");
    Require(r_loadForRenderer && r_loadForRenderer->current.enabled, "start",
            "r_loadForRenderer is off: zones would load without device objects");
    Require(!sys_smp_allowed->current.enabled && !r_smp_worker_thread[0]->current.enabled &&
                !r_smp_worker_thread[1]->current.enabled,
            "start", "the renderer is not configured for one thread");
    Require(fx_marks && !fx_marks->current.enabled, "start", "fx_marks is not off");
    // The free camera holds every frame's static models and sun shadow to rules
    // (world_draw_rules.h CheckSceneCounts, CheckSceneInView), so it refuses to
    // start with their switches off, as a command-line +set could leave them,
    // rather than pass without them. The draw mode keeps them as diagnostics.
    if (freeCamera)
        for (const dvar_s *dvar : {r_drawSModels, sm_enable, sm_sunEnable})
            Require(dvar && dvar->current.enabled, "start",
                    std::string(dvar ? dvar->name : "a draw switch") +
                        " is off; the free camera checks the static models and sun shadow it draws");
    Require(frontEndDataOut && frontEndDataOut->commands && frontEndDataOut->commands->cmds, "start",
            "the render commands are not set up (R_ConfigureRenderer did not run)");

    SelectWsi();
    KeepDxvkLogOnStderr();
    StartVideo();
    RequireDirect3D();

    // Windowed at a fixed client size: r_customMode is the engine's own way to ask
    // for a size outside the display mode list. Both dvars latch.
    SetBool(r_fullscreen, false);
    Dvar_MakeLatchedValueCurrent(const_cast<dvar_s *>(r_fullscreen));
    const std::string mode = std::to_string(kWindowWidth) + "x" + std::to_string(kWindowHeight);
    Dvar_SetString(const_cast<dvar_s *>(r_customMode), mode.c_str());
    Dvar_MakeLatchedValueCurrent(const_cast<dvar_s *>(r_customMode));

    // The renderer's own start, as R_BeginRegistration runs it.
    Say("world_draw_renderer=starting zones=queued_by_R_Init\n");
    R_Init();
    Require(dx.d3d9 && dx.device, "renderer", "R_Init returned without a device");
    Require(rg.registered, "renderer", "R_Init returned without registering the renderer (R_InitSystems)");
    Require(r_rendererInUse->current.integer == GFX_RENDERER_SHADER_3, "renderer",
            "R_PickRenderer did not choose the shader model 3 path (" +
                std::to_string(r_rendererInUse->current.integer) + ")");
    Require(vidConfig.displayWidth == unsigned(kWindowWidth) && vidConfig.displayHeight == unsigned(kWindowHeight) &&
                !vidConfig.isFullscreen,
            "renderer", "the window is not the windowed " + mode + " size");
    const HRESULT level = dx.device->TestCooperativeLevel();
    Require(level == D3D_OK, "renderer", "the device is not ready after R_Init: " + Hex(uint32_t(level)));
    D3DADAPTER_IDENTIFIER9 id{};
    char description[sizeof(id.Description) + 1]{};
    if (SUCCEEDED(dx.d3d9->GetAdapterIdentifier(dx.adapterIndex, 0, &id)))
        std::memcpy(description, id.Description, sizeof(id.Description));
    Say("world_draw_renderer=started renderer=shader_model_3 window=%s scene=%ux%u adapter=\"%s\" seconds=%.2f\n",
        mode.c_str(), vidConfig.sceneWidth, vidConfig.sceneHeight, description, Seconds(g_started));
}

FreeCameraInput Kisak_FreeCameraInput()
{
    return g_cameraInput;
}

int Kisak_FreeCameraSeconds()
{
    return g_cameraSeconds;
}

double WorldDrawStartSeconds()
{
    return Seconds(g_started);
}

void Kisak_DrawWorld(const std::filesystem::path &directory, const char *mapName)
{
    // The export entry point has synced the graphics zones.
    const double startSeconds = Seconds(g_started);
    RequireStartedRenderer();
    Say("world_draw_graphics_zones=loaded zones=%d seconds=%.2f\n", g_zoneCount, startSeconds);

    // 1. The map zone, then the map load the cgame does before its first frame
    // (CG_Init): the collision map as the client's CL_CM_LoadMap loads it when no
    // local server runs, then the renderer's own world registration (LoadWorld).
    // The renderer traces against the collision map while it lights models, so the
    // trace data CM_LoadMap sets up must exist before any scene is drawn.
    const auto mapStarted = std::chrono::steady_clock::now();
    const std::string worldName = LoadMap(mapName);
    const double mapSeconds = Seconds(mapStarted);
    Say("world_draw_map=loaded map=%s world=%s seconds=%.2f\n", mapName, worldName.c_str(), mapSeconds);

    // 2. The cameras.
    Camera a, b;
    ChooseCameras(*rgp.world, a, b);
    Say("world_draw_camera view=A classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", a.classname.c_str(),
        a.eye[0], a.eye[1], a.eye[2], a.angles[0], a.angles[1], a.angles[2]);
    Say("world_draw_camera view=B classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", b.classname.c_str(),
        b.eye[0], b.eye[1], b.eye[2], b.angles[0], b.angles[1], b.angles[2]);

    // The engine opens its own D3D9 call log (r_logFile > 0, dx.log in the working
    // directory) only after a frame it presents; opening it here records the
    // empty frame and the first world frame too.
    if (r_logFile && r_logFile->current.integer > 0)
    {
        RB_OpenLogFile();
        Say("world_draw_diag log=dx.log frames=%d\n", r_logFile->current.integer);
    }

    // 3. The renderer's first frame. CL_InitDedicated's R_ConfigureRenderer set up
    // the frame data before the device existed, so the backend's pre-tessellated
    // index buffer for the coming frame is still unset (R_ToggleSmpFrame copied a
    // buffer that did not exist yet), while R_Init's R_InitRenderBuffers created
    // the buffers and pointed the front end at one of them. A world frame drawn
    // now would write its batched world surfaces into one buffer and draw them
    // from none, and come out dark (docs/linux-world-draw-probe.md has the GPU
    // runs that showed it). A client never draws a world
    // as its first frame: it presents its menu and loading screen first, and
    // each frame's R_IssueRenderCommands moves both pointers on together. One
    // empty frame does the same here.
    const bool agreedBefore = PreTessBuffersAgree();
    PresentEmptyFrame();
    Require(PreTessBuffersAgree(), "frame",
            "after the empty frame the backend's pre-tessellated index buffer is still not the front end's");
    Say("world_draw_first_frame=empty pretess_ib_before=%s pretess_ib_after=agreed\n",
        agreedBefore ? "agreed" : "differed");

    // 4. The frames: view A until it settles, then view B until it settles.
    auto refdef = std::make_unique<refdef_s>();
    auto drawUntilSettled = [&](const Camera &camera, const char *label, std::vector<FrameResult> &frames,
                                std::vector<uint64_t> &digests) {
        for (int draw = 0; draw < kMaxSettleDraws; ++draw)
        {
            if (RB_IsLogging())
            {
                char marker[96];
                std::snprintf(marker, sizeof(marker), "===== world_draw_probe view %s draw %d =====\n", label, draw);
                RB_LogPrint(marker);
            }
            frames.push_back(DrawView(camera, *refdef));
            digests.push_back(frames.back().stats.digest);
            SayFrame(label, draw, frames.back());
            if (wdr::CheckSettled(digests, frames.back().smcPatches).empty())
                break;
        }
        Say("world_draw_view view=%s draws=%zu settled=%s\n", label, frames.size(),
            wdr::CheckSettled(digests, frames.back().smcPatches).empty() ? "yes" : "no");
    };
    std::vector<FrameResult> framesA, framesB;
    std::vector<uint64_t> digestsA, digestsB;
    drawUntilSettled(a, "A", framesA, digestsA);
    drawUntilSettled(b, "B", framesB, digestsB);
    const FrameResult &frameB = framesB.back();

    // 5. The rules.
    std::vector<std::string> problems;
    auto check = [&](const std::string &what, const std::string &problem) {
        if (!problem.empty())
            problems.push_back(what + ": " + problem);
    };
    check("view A", wdr::CheckFrame(framesA.back().stats, vidConfig.sceneWidth, vidConfig.sceneHeight));
    check("view A", wdr::CheckDrawCounts(framesA.back().counts));
    check("view A", wdr::CheckSettled(digestsA, framesA.back().smcPatches));
    check("view B", wdr::CheckFrame(frameB.stats, vidConfig.sceneWidth, vidConfig.sceneHeight));
    check("view B", wdr::CheckDrawCounts(frameB.counts));
    check("view B", wdr::CheckSettled(digestsB, frameB.smcPatches));
    check("views A and B", wdr::CheckViewsDiffer(framesA.back().stats.digest, frameB.stats.digest));

    // 6. Optionally keep view A on screen. Each frame presents; nothing is saved.
    const int showSeconds = g_showSeconds;
    size_t shownFrames = 0;
    if (showSeconds > 0 && problems.empty())
    {
        const auto showStarted = std::chrono::steady_clock::now();
        while (Seconds(showStarted) < double(showSeconds))
        {
            DrawView(a, *refdef);
            ++shownFrames;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Say("world_draw_shown seconds=%d frames=%zu\n", showSeconds, shownFrames);
    }
    const double seconds = Seconds(g_started);

    // 7. The report: the cameras, counts and measurements. No pixels.
    std::ostringstream json;
    json << "{\"schema\":1,\"tool\":\"world_draw_probe\",\"map\":" << Json(mapName)
         << ",\"world\":" << Json(worldName) << ",\"renderer\":\"shader_model_3\",\"window\":[" << kWindowWidth
         << "," << kWindowHeight << "],\"scene\":[" << vidConfig.sceneWidth << "," << vidConfig.sceneHeight
         << "],\"half_fov_x_degrees\":" << kHalfFovXDegrees << ",\"scene_time\":" << kSceneTime
         << ",\"cameras\":{\"A\":" << CameraJson(a) << ",\"B\":" << CameraJson(b) << "},\"frames\":{\"A\":[";
    for (size_t i = 0; i < framesA.size(); ++i)
        json << (i ? "," : "") << FrameJson(framesA[i]);
    json << "],\"B\":[";
    for (size_t i = 0; i < framesB.size(); ++i)
        json << (i ? "," : "") << FrameJson(framesB[i]);
    json << "]},\"max_settle_draws\":" << kMaxSettleDraws << ",\"shown_frames\":" << shownFrames
         << ",\"start_seconds\":" << startSeconds << ",\"map_seconds\":" << mapSeconds << ",\"seconds\":" << seconds
         << ",\"problem_count\":" << problems.size() << ",\"problems\":[";
    for (size_t i = 0; i < problems.size(); ++i)
        json << (i ? "," : "") << "\n" << Json(problems[i]);
    json << "]}\n";
    WriteExclusiveFile(directory / "world-draw-probe.json", json.str());

    for (const std::string &problem : problems)
        Say("world_draw_problem %s\n", problem.c_str());
    Say("world_draw_probe=%s problems=%zu seconds=%.2f\n", problems.empty() ? "passed" : "failed", problems.size(),
        seconds);
    Require(problems.empty(), "check", std::to_string(problems.size()) + " frame rule(s) failed (see world_draw_problem lines)");
}
