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
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/rb_stats.h>
#include <linux/d3d9_types.h>
#include <linux/client/window/sdl_api.h>

#include "world_draw_probe.h"
#include "world_draw_rules.h"
#include "world_frontend_invariants.h"
#include "world_surface_export.h"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// The zone table (src/database/db_registry.cpp). Read only.
extern int32_t g_zoneCount;

namespace
{
namespace wf = kisak_world_frontend;
namespace wdr = kisak_world_draw;

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 480;
// cg_fov's default, 65 degrees across a 4:3 view, as CG_CalcFov turns it into
// the refdef's tangents.
constexpr float kHalfFovXDegrees = 32.5f;
// The scene time every frame is drawn at. R_GenerateReflectionRawData draws at
// 0 too; one fixed time makes a repeated draw of a view comparable.
constexpr int kSceneTime = 0;
// View A is drawn this many times: the first draw may differ from later ones
// while per-frame caches settle, and the last two must match.
constexpr int kRepeatDraws = 3;
constexpr size_t kMaxSpawnViews = 256;
constexpr int kMaxShowSeconds = 60;

[[gnu::format(printf, 1, 2)]] void Say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

[[noreturn]] void Fail(const char *stage, const std::string &reason)
{
    std::fflush(stdout);
    throw std::runtime_error(std::string("world_draw_probe stage=") + stage + ": " + reason);
}

void Require(bool ok, const char *stage, const std::string &why)
{
    if (!ok)
        Fail(stage, why);
}

std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08" PRIX32, value);
    return text;
}

std::string Hex64(uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%016" PRIx64, value);
    return text;
}

std::string Json(const std::string &text)
{
    std::string out = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c < 0x20 || c >= 0x7F)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        }
        else
            out += char(c);
    }
    return out + "\"";
}

// The report is created exclusively with owner-only permissions, inside the
// private directory the export entry point already checked.
void WriteExclusiveFile(const std::filesystem::path &file, const std::string &text)
{
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    Require(fd >= 0, "report", "cannot create " + file.string() + ": " + std::strerror(errno));
    size_t written = 0;
    while (written < text.size())
    {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            ::close(fd);
            Fail("report", "cannot write " + file.string());
        }
        written += size_t(n);
    }
    Require(::close(fd) == 0, "report", "cannot close " + file.string());
}

void SetBool(const dvar_s *dvar, bool value)
{
    Dvar_SetBool(const_cast<dvar_s *>(dvar), value);
}

double Seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
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

struct Camera
{
    std::string classname;
    float eye[3] = {};
    float angles[3] = {};
};

struct FrameResult
{
    wdr::FrameStats stats;
    wdr::DrawCounts counts;
    double seconds = 0;
};

// What R_GetBackBufferData (src/gfx_d3d/r_screenshot.cpp) reads after the
// engine's cubemap frames: the back buffer, which a windowed device with one
// back buffer and D3DSWAPEFFECT_DISCARD keeps through Present (DXVK keeps it
// too: d3d9_swapchain.h, SwapWithFrontBuffer). Copied to system memory, locked
// read-only and measured; the pixels are not kept.
wdr::FrameStats ReadBackBuffer()
{
    IDirect3DSurface9 *backBuffer = nullptr;
    HRESULT hr = dx.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    Require(SUCCEEDED(hr) && backBuffer, "readback", "GetBackBuffer failed: " + Hex(uint32_t(hr)));
    D3DSURFACE_DESC desc{};
    hr = backBuffer->GetDesc(&desc);
    if (FAILED(hr) || (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8))
    {
        backBuffer->Release();
        Fail("readback", "the back buffer's format " + std::to_string(int(desc.Format)) +
                             " is not X8R8G8B8 or A8R8G8B8 (" + Hex(uint32_t(hr)) + ")");
    }
    IDirect3DSurface9 *copy = nullptr;
    hr = dx.device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy,
                                                nullptr);
    if (FAILED(hr) || !copy)
    {
        backBuffer->Release();
        Fail("readback", "CreateOffscreenPlainSurface failed: " + Hex(uint32_t(hr)));
    }
    hr = dx.device->GetRenderTargetData(backBuffer, copy);
    backBuffer->Release();
    if (FAILED(hr))
    {
        copy->Release();
        Fail("readback", "GetRenderTargetData failed: " + Hex(uint32_t(hr)));
    }
    D3DLOCKED_RECT locked{};
    hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr) || !locked.pBits || locked.Pitch < INT(desc.Width * 4))
    {
        copy->Release();
        Fail("readback", "LockRect failed: " + Hex(uint32_t(hr)));
    }
    const wdr::FrameStats stats = wdr::MeasureFrame(static_cast<const uint8_t *>(locked.pBits), desc.Width,
                                                    desc.Height, size_t(locked.Pitch));
    copy->UnlockRect();
    copy->Release();
    return stats;
}

// One frame, call for call as R_GenerateReflectionRawData draws a cubemap face,
// without the effects update (no effects system runs here) and with the view
// values the cgame's CG_CalcViewValues would give a player standing at the
// camera: the whole scene viewport, cg_fov's default, zNear 0.
FrameResult DrawView(const Camera &camera, refdef_s &refdef)
{
    const auto started = std::chrono::steady_clock::now();
    std::memset(&refdef, 0, sizeof(refdef));
    refdef.vieworg[0] = camera.eye[0];
    refdef.vieworg[1] = camera.eye[1];
    refdef.vieworg[2] = camera.eye[2];
    refdef.localClientNum = 0;
    refdef.time = kSceneTime;
    refdef.blurRadius = 0.0f;
    refdef.useScissorViewport = false;
    R_InitPrimaryLights(refdef.primaryLights);

    R_BeginFrame();
    R_BeginSharedCmdList();
    R_ClearClientCmdList2D();
    R_ClearScene(0);
    refdef.x = 0;
    refdef.y = 0;
    refdef.width = vidConfig.sceneWidth;
    refdef.height = vidConfig.sceneHeight;
    refdef.tanHalfFovX = std::tan(kHalfFovXDegrees * float(M_PI) / 180.0f);
    refdef.tanHalfFovY = refdef.tanHalfFovX * float(refdef.height) / float(refdef.width);
    refdef.zNear = 0.0f;
    AnglesToAxis(camera.angles, refdef.viewaxis);
    R_SetLodOrigin(&refdef);
    R_RenderScene(&refdef);

    // What the front end emitted for this view, read before R_IssueRenderCommands
    // hands the frame's data to the backend and flips to the other buffer.
    FrameResult result;
    Require(frontEndDataOut && frontEndDataOut->viewInfoCount == 1, "frame",
            "R_RenderScene did not produce exactly one view");
    const GfxViewInfo &view = frontEndDataOut->viewInfo[0];
    result.counts.litSurfaces = view.litInfo.drawSurfCount;
    result.counts.decalSurfaces = view.decalInfo.drawSurfCount;
    result.counts.emissiveSurfaces = view.emissiveInfo.drawSurfCount;

    R_EndFrame();
    R_IssueRenderCommands(0xFFFFFFFF);

    // RB_BeginFrame zeroed the counters at the start of this frame's backend work.
    // viewStats[0] counts draws into the scene's own targets; viewStats[1] counts
    // the shadow maps (R_UpdateStatsTarget), which are not the camera's view.
    const GfxPrimStats &world = g_frameStatsCur.viewStats[0].primStats[GFX_PRIM_STATS_WORLD];
    result.counts.worldDrawCalls = uint32_t(world.primCount);
    result.counts.worldTriangles = uint32_t(world.triCount);
    const HRESULT level = dx.device->TestCooperativeLevel();
    Require(level == D3D_OK, "frame", "the device is not ready after the frame: " + Hex(uint32_t(level)));
    result.stats = ReadBackBuffer();
    result.seconds = Seconds(started);
    return result;
}

// ---- the map ----------------------------------------------------------------

template <class Asset> struct Collected
{
    std::vector<Asset *> items;
};

template <class Asset> void Collect(XAssetHeader header, void *context)
{
    static_cast<Collected<Asset> *>(context)->items.push_back(reinterpret_cast<Asset *>(header.data));
}

GfxWorld *FindPackagedWorld(const char *mapName)
{
    Collected<GfxWorld> worlds;
    DB_EnumXAssets(ASSET_TYPE_GFXWORLD, Collect<GfxWorld>, &worlds, true);
    GfxWorld *world = nullptr;
    unsigned packaged = 0;
    for (GfxWorld *candidate : worlds.items)
    {
        if (!candidate || !candidate->name || DB_IsXAssetDefault(ASSET_TYPE_GFXWORLD, candidate->name))
            continue;
        ++packaged;
        world = candidate;
    }
    Require(packaged == 1, "world",
            "expected exactly one packaged GFXWORLD in the map zone, found " + std::to_string(packaged));
    const std::string name = world->name;
    const std::string baseName = world->baseName ? world->baseName : "";
    Require(name.find(mapName) != std::string::npos || baseName.find(mapName) != std::string::npos, "world",
            "loaded GFXWORLD '" + name + "' does not name the requested map '" + mapName + "'");
    return world;
}

const MapEnts *FindMapEnts(const char *worldName)
{
    Collected<MapEnts> ents;
    DB_EnumXAssets(ASSET_TYPE_MAP_ENTS, Collect<MapEnts>, &ents, true);
    const MapEnts *found = nullptr;
    for (const MapEnts *candidate : ents.items)
    {
        if (!candidate || !candidate->name || std::strcmp(candidate->name, worldName))
            continue;
        Require(!found, "cameras", std::string("more than one MAP_ENTS asset is named ") + worldName);
        found = candidate;
    }
    Require(found != nullptr, "cameras", std::string("no MAP_ENTS asset is named ") + worldName);
    Require(found->entityString != nullptr && found->numEntityChars > 0, "cameras",
            "the map's entity string is empty");
    return found;
}

// View A is the map author's intermission camera, the map's own overview, when
// the map has one; otherwise the first player spawn. View B is the first player
// spawn that is not view A. Both come from the entity string in file order, with
// the eye rule of the world front-end probe (world_frontend_invariants.h).
void ChooseCameras(const GfxWorld &world, Camera &a, Camera &b)
{
    const MapEnts *ents = FindMapEnts(world.name);
    const wf::SpawnScan scan = wf::ScanSpawnViews(ents->entityString, size_t(ents->numEntityChars), kMaxSpawnViews);
    Require(scan.error.empty(), "cameras", "entity string: " + scan.error);
    const wf::SpawnView *intermission = nullptr;
    std::vector<const wf::SpawnView *> spawns;
    for (const wf::SpawnView &view : scan.views)
    {
        if (view.classname == "mp_global_intermission")
        {
            if (!intermission)
                intermission = &view;
        }
        else
            spawns.push_back(&view);
    }
    Require(!spawns.empty(), "cameras", "the map has no player spawn");
    auto copy = [](const wf::SpawnView &from, Camera &to) {
        to.classname = from.classname;
        for (int i = 0; i < 3; ++i)
            to.eye[i] = from.eye[i], to.angles[i] = from.angles[i];
    };
    copy(intermission ? *intermission : *spawns[0], a);
    copy(intermission ? *spawns[0] : *spawns[spawns.size() > 1 ? 1 : 0], b);
    Require(std::memcmp(a.eye, b.eye, sizeof(a.eye)) || std::memcmp(a.angles, b.angles, sizeof(a.angles)), "cameras",
            "the map has only one distinct camera");
}

void RequireCodeImages()
{
    const struct
    {
        const char *name;
        const GfxImage *image;
    } images[] = {{"white", rgp.whiteImage}, {"black", rgp.blackImage}, {"identity normal map", rgp.identityNormalMapImage}};
    for (const auto &entry : images)
        Require(entry.image && entry.image->texture.basemap, "renderer",
                std::string("R_InitSystems left the code image '") + entry.name + "' without a texture");
}

std::string CameraJson(const Camera &camera)
{
    std::ostringstream json;
    json << "{\"classname\":" << Json(camera.classname) << ",\"eye\":[" << camera.eye[0] << "," << camera.eye[1]
         << "," << camera.eye[2] << "],\"angles\":[" << camera.angles[0] << "," << camera.angles[1] << ","
         << camera.angles[2] << "]}";
    return json.str();
}

std::string FrameJson(const FrameResult &frame)
{
    const wdr::FrameStats &s = frame.stats;
    const wdr::DrawCounts &c = frame.counts;
    std::ostringstream json;
    char share[32], luma[32], deviation[32];
    std::snprintf(share, sizeof(share), "%.4f", s.dominantShare);
    std::snprintf(luma, sizeof(luma), "%.2f", s.meanLuma);
    std::snprintf(deviation, sizeof(deviation), "%.2f", s.lumaStdDev);
    json << "{\"width\":" << s.width << ",\"height\":" << s.height << ",\"distinct_colors\":" << s.distinctColors
         << ",\"dominant_share\":" << share << ",\"mean_luma\":" << luma << ",\"luma_std_dev\":" << deviation
         << ",\"detail_tiles\":" << s.detailTiles << ",\"digest\":\"" << Hex64(s.digest) << "\""
         << ",\"lit_surfaces\":" << c.litSurfaces << ",\"decal_surfaces\":" << c.decalSurfaces
         << ",\"emissive_surfaces\":" << c.emissiveSurfaces << ",\"world_draw_calls\":" << c.worldDrawCalls
         << ",\"world_triangles\":" << c.worldTriangles << ",\"seconds\":" << frame.seconds << "}";
    return json.str();
}

void SayFrame(const char *label, int draw, const FrameResult &frame)
{
    const wdr::FrameStats &s = frame.stats;
    const wdr::DrawCounts &c = frame.counts;
    Say("world_draw_frame view=%s draw=%d size=%ux%u colors=%zu dominant=%.3f luma=%.1f+-%.1f detail_tiles=%u "
        "digest=%s lit=%u decal=%u emissive=%u world_calls=%u world_tris=%u seconds=%.3f\n",
        label, draw, s.width, s.height, s.distinctColors, s.dominantShare, s.meanLuma, s.lumaStdDev, s.detailTiles,
        Hex64(s.digest).c_str(), c.litSurfaces, c.decalSurfaces, c.emissiveSurfaces, c.worldDrawCalls,
        c.worldTriangles, frame.seconds);
}

std::chrono::steady_clock::time_point g_started;
// KISAK_WORLD_DRAW_SHOW_SECONDS, read with the other arguments before any device work.
int g_showSeconds = 0;
} // namespace

// The client's renderer configuration (src/client_mp/cl_main_mp.cpp SetupGfxConfig
// and CL_SetFastFileNames) with the dedicated server's zone set (no ui_mp), for a
// renderer that runs on this thread alone. Unlike the dedicated server's
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
    SetGfxConfig(&config);
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

void Kisak_StartWorldDraw(const char *mapName, const wf::Layout &loaderLayout)
{
    g_started = std::chrono::steady_clock::now();
    Require(mapName && kisak_world_inventory::IsAllowedMapName(mapName), "arguments",
            "KISAK_WORLD_DRAW_MAP must name an allowlisted map");
    if (const char *show = std::getenv("KISAK_WORLD_DRAW_SHOW_SECONDS"))
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

void Kisak_DrawWorld(const std::filesystem::path &directory, const char *mapName)
{
    // The export entry point has synced the graphics zones.
    const double startSeconds = Seconds(g_started);
    Require(dx.device != nullptr && rg.registered, "start", "Kisak_StartWorldDraw did not run");
    Require(g_zoneCount > 0, "graphics", "no graphics zone loaded");
    RequireCodeImages();
    Say("world_draw_graphics_zones=loaded zones=%d seconds=%.2f\n", g_zoneCount, startSeconds);

    // 1. The map zone, the fast-file branch of a map load, then the renderer's own
    // world registration.
    const auto mapStarted = std::chrono::steady_clock::now();
    XZoneInfo zone{mapName, DB_ZONE_GAME, DB_ZONE_GAME};
    DB_LoadXAssets(&zone, 1, 1);
    DB_SyncXAssets();
    GfxWorld *packaged = FindPackagedWorld(mapName);
    std::string worldName = packaged->name;
    int checksum = 0;
    R_LoadWorld(worldName.data(), &checksum, 0);
    Require(rgp.world == packaged, "world", "R_LoadWorld did not register the packaged world");
    R_EndRegistration();
    const double mapSeconds = Seconds(mapStarted);
    Say("world_draw_map=loaded map=%s world=%s seconds=%.2f\n", mapName, worldName.c_str(), mapSeconds);

    // 2. The cameras.
    Camera a, b;
    ChooseCameras(*rgp.world, a, b);
    Say("world_draw_camera view=A classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", a.classname.c_str(),
        a.eye[0], a.eye[1], a.eye[2], a.angles[0], a.angles[1], a.angles[2]);
    Say("world_draw_camera view=B classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", b.classname.c_str(),
        b.eye[0], b.eye[1], b.eye[2], b.angles[0], b.angles[1], b.angles[2]);

    // 3. The frames: view A until it repeats, then view B once.
    auto refdef = std::make_unique<refdef_s>();
    std::vector<FrameResult> framesA;
    std::vector<uint64_t> digestsA;
    for (int draw = 0; draw < kRepeatDraws; ++draw)
    {
        framesA.push_back(DrawView(a, *refdef));
        digestsA.push_back(framesA.back().stats.digest);
        SayFrame("A", draw, framesA.back());
    }
    const FrameResult frameB = DrawView(b, *refdef);
    SayFrame("B", 0, frameB);

    // 4. The rules.
    std::vector<std::string> problems;
    auto check = [&](const std::string &what, const std::string &problem) {
        if (!problem.empty())
            problems.push_back(what + ": " + problem);
    };
    check("view A", wdr::CheckFrame(framesA.back().stats, vidConfig.sceneWidth, vidConfig.sceneHeight));
    check("view A", wdr::CheckDrawCounts(framesA.back().counts));
    check("view A", wdr::CheckRepeatable(digestsA));
    check("view B", wdr::CheckFrame(frameB.stats, vidConfig.sceneWidth, vidConfig.sceneHeight));
    check("view B", wdr::CheckDrawCounts(frameB.counts));
    check("views A and B", wdr::CheckViewsDiffer(framesA.back().stats.digest, frameB.stats.digest));

    // 5. Optionally keep view A on screen. Each frame presents; nothing is saved.
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

    // 6. The report: the cameras, counts and measurements. No pixels.
    std::ostringstream json;
    json << "{\"schema\":1,\"tool\":\"world_draw_probe\",\"map\":" << Json(mapName)
         << ",\"world\":" << Json(worldName) << ",\"renderer\":\"shader_model_3\",\"window\":[" << kWindowWidth
         << "," << kWindowHeight << "],\"scene\":[" << vidConfig.sceneWidth << "," << vidConfig.sceneHeight
         << "],\"half_fov_x_degrees\":" << kHalfFovXDegrees << ",\"scene_time\":" << kSceneTime
         << ",\"cameras\":{\"A\":" << CameraJson(a) << ",\"B\":" << CameraJson(b) << "},\"frames\":{\"A\":[";
    for (size_t i = 0; i < framesA.size(); ++i)
        json << (i ? "," : "") << FrameJson(framesA[i]);
    json << "],\"B\":[" << FrameJson(frameB) << "]},\"shown_frames\":" << shownFrames
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
