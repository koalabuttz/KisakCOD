// The engine calls the world draw probe and the free-camera adapter share: see
// world_draw_engine.h. Moved out of world_draw_probe.cpp unchanged in behaviour,
// with the map's unload added.

#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_bsp.h>
#include <win32/win_local.h>
#include <database/database.h>
#include <client_mp/client_mp.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_debug.h>
#include <gfx_d3d/r_draw_method.h>
#include <gfx_d3d/r_drawsurf.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/rb_stats.h>
#include <linux/d3d9_types.h>

#include "world_draw_engine.h"
#include "world_frontend_invariants.h"
#include "world_surface_export.h"

#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

// The zone table (src/database/db_registry.cpp). Read only.
extern int32_t g_zoneCount;
extern XZone g_zones[];
extern uint8_t g_zoneHandles[];
// When each renderer warning may print again (src/gfx_d3d/r_warn.cpp): zero
// until R_WarnOncePerFrame first prints it. Read only.
extern uint32_t s_warnCount[R_WARN_COUNT];

namespace kisak_world_draw_engine
{
namespace
{
namespace wf = kisak_world_frontend;

constexpr size_t kMaxSpawnViews = 256;
const char *g_tool = "world_draw_probe";

template <class Asset> struct Collected
{
    std::vector<Asset *> items;
};

template <class Asset> void Collect(XAssetHeader header, void *context)
{
    static_cast<Collected<Asset> *>(context)->items.push_back(reinterpret_cast<Asset *>(header.data));
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

int GameZoneCount()
{
    int count = 0;
    for (int i = 0; i < g_zoneCount; ++i)
        if (g_zones[g_zoneHandles[i]].flags & DB_ZONE_GAME)
            ++count;
    return count;
}
} // namespace

// ---- reporting --------------------------------------------------------------

void SetTool(const char *tool)
{
    g_tool = tool;
}

const char *Tool()
{
    return g_tool;
}

void Say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

void Fail(const char *stage, const std::string &reason)
{
    std::fflush(stdout);
    throw std::runtime_error(std::string(g_tool) + " stage=" + stage + ": " + reason);
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

double Seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// ---- the frame --------------------------------------------------------------

void RequireStartedRenderer()
{
    Require(dx.device != nullptr && rg.registered, "start", "Kisak_StartWorldDraw did not run");
    Require(g_zoneCount > 0, "graphics", "no graphics zone loaded");
    const struct
    {
        const char *name;
        const GfxImage *image;
    } images[] = {{"white", rgp.whiteImage}, {"black", rgp.blackImage}, {"identity normal map", rgp.identityNormalMapImage}};
    for (const auto &entry : images)
        Require(entry.image && entry.image->texture.basemap, "renderer",
                std::string("R_InitSystems left the code image '") + entry.name + "' without a texture");
}

void BackBufferSize(uint32_t &width, uint32_t &height)
{
    IDirect3DSurface9 *backBuffer = nullptr;
    HRESULT hr = dx.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    Require(SUCCEEDED(hr) && backBuffer, "readback", "GetBackBuffer failed: " + Hex(uint32_t(hr)));
    D3DSURFACE_DESC desc{};
    hr = backBuffer->GetDesc(&desc);
    backBuffer->Release();
    Require(SUCCEEDED(hr), "readback", "the back buffer has no description: " + Hex(uint32_t(hr)));
    width = desc.Width;
    height = desc.Height;
}

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

// The front end writes the indices of the world's batched surfaces into
// gfxBuf.preTessIndexBuffer (R_BeginPreTess, R_AllocPreTessIndices), and
// R_TessTrianglesPreTessList binds the frame data's preTessIb. R_ToggleSmpFrame
// moves both to the next buffer of the pair at the end of every frame.
bool PreTessBuffersAgree()
{
    const GfxIndexBufferState *filled = gfxBuf.preTessIndexBuffer;
    return filled && filled->buffer && frontEndDataOut && frontEndDataOut->preTessIb == filled->buffer;
}

void PresentEmptyFrame()
{
    R_BeginFrame();
    R_BeginSharedCmdList();
    R_ClearClientCmdList2D();
    R_EndFrame();
    R_IssueRenderCommands(0xFFFFFFFF);
    const HRESULT level = dx.device->TestCooperativeLevel();
    Require(level == D3D_OK, "frame", "the device is not ready after the empty frame: " + Hex(uint32_t(level)));
}

void PrepareRefdef(refdef_s &refdef)
{
    std::memset(&refdef, 0, sizeof(refdef));
    refdef.localClientNum = 0;
    refdef.blurRadius = 0.0f;
    refdef.useScissorViewport = false;
    R_InitPrimaryLights(refdef.primaryLights);
}

FrameResult DrawRefdef(refdef_s &refdef, bool readBack, const std::function<void(const GfxViewInfo &)> &inspect)
{
    const auto started = std::chrono::steady_clock::now();
    Require(PreTessBuffersAgree(), "frame",
            "the backend would draw the pre-tessellated surfaces from another index buffer than the one the front "
            "end fills");
    R_BeginFrame();
    R_BeginSharedCmdList();
    R_ClearClientCmdList2D();
    R_ClearScene(0);
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
    result.smcPatches = uint32_t(frontEndDataOut->smcPatchCount);
    if (inspect)
        inspect(view);

    // The static models and the sun's shadow map, as the front end left them.
    // R_AddAllStaticModelSurfacesCamera clears the visibility byte of every static
    // model it culls or cannot light, so the ones still set are the ones it kept.
    wdr::SceneCounts &inScene = result.scene;
    const GfxWorldDpvsStatic &dpvs = rgp.world->dpvs;
    inScene.staticModels = dpvs.smodelCount;
    for (uint32_t i = 0; i < dpvs.smodelCount; ++i)
        inScene.staticModelsDrawn += dpvs.smodelVisData[0][i] != 0;
    inScene.staticModelSurfaces = uint32_t(scene.drawSurfCount[DRAW_SURF_SMODEL_CAMERA_LIT] +
                                           scene.drawSurfCount[DRAW_SURF_SMODEL_CAMERA_DECAL] +
                                           scene.drawSurfCount[DRAW_SURF_SMODEL_CAMERA_EMISSIVE]);
    const uint32_t sun = rgp.world->sunPrimaryLightIndex;
    inScene.sunLight = sun != 0;
    inScene.sunUsed = sun != 0 && ((scene.shadowableLightIsUsed[sun >> 5] >> (sun & 31)) & 1u) != 0;
    inScene.shadowMaps = sm_enable->current.enabled && gfxDrawMethod.drawScene == GFX_DRAW_SCENE_STANDARD;
    inScene.sunShadowDrawn = rg.drawSunShadow != 0;
    if (inScene.sunShadowDrawn)
    {
        // Only a frame that set the shadow map up wrote its partitions.
        inScene.sunShadowSurfaces =
            view.sunShadow.partition[0].info.drawSurfCount + view.sunShadow.partition[1].info.drawSurfCount;
        inScene.sunShadowStaticModelSurfaces = uint32_t(scene.drawSurfCount[DRAW_SURF_SMODEL_SUNSHADOW_0] +
                                                        scene.drawSurfCount[DRAW_SURF_SMODEL_SUNSHADOW_1]);
    }

    R_EndFrame();
    R_IssueRenderCommands(0xFFFFFFFF);

    // RB_BeginFrame zeroed the counters at the start of this frame's backend work.
    // R_UpdateStatsTarget sends draws into the shadow cookie and dynamic shadow
    // targets to viewStats[1] and every other draw, the sun's shadow map
    // included, to viewStats[0]. The world counts are viewStats[0]'s; the static
    // model counts take both, so they include the static models drawn into the
    // sun's shadow map.
    const GfxPrimStats &world = g_frameStatsCur.viewStats[0].primStats[GFX_PRIM_STATS_WORLD];
    result.counts.worldDrawCalls = uint32_t(world.primCount);
    result.counts.worldTriangles = uint32_t(world.triCount);
    for (const GfxViewStats &stats : g_frameStatsCur.viewStats)
        for (int kind : {GFX_PRIM_STATS_SMODELCACHED, GFX_PRIM_STATS_SMODELRIGID})
        {
            inScene.staticModelDrawCalls += uint32_t(stats.primStats[kind].primCount);
            inScene.staticModelTriangles += uint32_t(stats.primStats[kind].triCount);
        }
    const HRESULT level = dx.device->TestCooperativeLevel();
    Require(level == D3D_OK, "frame", "the device is not ready after the frame: " + Hex(uint32_t(level)));
    if (readBack)
        result.stats = ReadBackBuffer();
    result.seconds = Seconds(started);
    return result;
}

std::vector<std::string> DroppedDrawWarnings()
{
    // Each warning whose message (s_warnFormat, src/gfx_d3d/r_warn.cpp) says the
    // renderer left something out: a model, surface, mesh, effect element or light.
    static constexpr struct
    {
        GfxWarningType type;
        const char *name;
    } kDropping[] = {
        {R_WARN_FRONTEND_ENT_LIMIT, "R_WARN_FRONTEND_ENT_LIMIT"},
        {R_WARN_MODEL_LIGHT_CACHE, "R_WARN_MODEL_LIGHT_CACHE"},
        {R_WARN_TEMP_SKIN_BUF_SIZE, "R_WARN_TEMP_SKIN_BUF_SIZE"},
        {R_WARN_MAX_SKINNED_CACHE_VERTICES, "R_WARN_MAX_SKINNED_CACHE_VERTICES"},
        {R_WARN_MAX_SCENE_SURFS_SIZE, "R_WARN_MAX_SCENE_SURFS_SIZE"},
        {R_WARN_MAX_CLOUDS, "R_WARN_MAX_CLOUDS"},
        {R_WARN_SMODEL_LIGHTING, "R_WARN_SMODEL_LIGHTING"},
        {R_WARN_MAX_DRAWSURFS, "R_WARN_MAX_DRAWSURFS"},
        {R_WARN_GFX_CODE_MESH_LIMIT, "R_WARN_GFX_CODE_MESH_LIMIT"},
        {R_WARN_GFX_MARK_MESH_LIMIT, "R_WARN_GFX_MARK_MESH_LIMIT"},
        {R_WARN_MAX_SCENE_DRAWSURFS, "R_WARN_MAX_SCENE_DRAWSURFS"},
        {R_WARN_MAX_FX_DRAWSURFS, "R_WARN_MAX_FX_DRAWSURFS"},
        {R_WARN_PRIM_DRAW_SURF_BUFFER_SIZE, "R_WARN_PRIM_DRAW_SURF_BUFFER_SIZE"},
        {R_WARN_CMDBUF_OVERFLOW, "R_WARN_CMDBUF_OVERFLOW"},
        {R_WARN_SPOT_LIGHT_LIMIT, "R_WARN_SPOT_LIGHT_LIMIT"},
        {R_WARN_FX_ELEM_LIMIT, "R_WARN_FX_ELEM_LIMIT"},
    };
    std::vector<std::string> fired;
    for (const auto &warning : kDropping)
        if (s_warnCount[warning.type] != 0)
            fired.push_back(warning.name);
    return fired;
}

// ---- the map ----------------------------------------------------------------

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

std::string LoadMap(const char *mapName)
{
    Require(mapName && kisak_world_inventory::IsAllowedMapName(mapName), "world",
            "the map to load must be an allowlisted map");
    Require(!MapHeld(), "world", "a map is already loaded");
    // The renderer traces against the collision map while it lights models, so the
    // trace data CM_LoadMap sets up must exist before any scene is drawn.
    XZoneInfo zone{mapName, DB_ZONE_GAME, DB_ZONE_GAME};
    DB_LoadXAssets(&zone, 1, 1);
    DB_SyncXAssets();
    Require(GameZoneCount() == 1, "world", "the map zone did not load");
    GfxWorld *packaged = FindPackagedWorld(mapName);
    std::string worldName = packaged->name;
    Require(com_sv_running && !com_sv_running->current.enabled, "world", "a local server is running");
    int collisionChecksum = 0;
    CM_LoadMap(worldName.c_str(), &collisionChecksum);
    CM_LinkWorld();
    Com_LoadWorld(worldName.data());
    Require(cm.isInUse && cm.name && worldName == cm.name, "world",
            "CM_LoadMap did not load the map's collision map");
    Require(comWorld.isInUse && comWorld.name && worldName == comWorld.name, "world",
            "Com_LoadWorld did not find the map's world data");
    int checksum = 0;
    R_LoadWorld(worldName.data(), &checksum, 0);
    Require(rgp.world == packaged, "world", "R_LoadWorld did not register the packaged world");
    R_EndRegistration();
    return worldName;
}

bool MapHeld()
{
    return rgp.world || comWorld.isInUse || cm.isInUse || GameZoneCount() > 0;
}

void UnloadMap()
{
    // R_Shutdown's world part and CL_ShutdownRenderer's (Com_ShutdownWorld, then
    // CM_Shutdown), without shutting the renderer down. DB_RemoveGfxWorld,
    // CM_Unload and Com_UnloadWorld refuse a world still in use, so these come
    // before the zone.
    if (rgp.world)
        R_ShutdownWorld();
    if (comWorld.isInUse)
        Com_ShutdownWorld();
    if (cm.isInUse)
        CM_Shutdown();
    if (GameZoneCount() > 0)
    {
        // A zone list with no name loads nothing: only the free flags act, as the
        // client's next map load would free this one (CL_InitCGame).
        XZoneInfo zone{nullptr, 0, DB_ZONE_GAME};
        DB_LoadXAssets(&zone, 1, 0);
        DB_SyncXAssets();
    }
    Require(!MapHeld(), "world", "the map is still held after its unload");
}

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
    const wdr::SceneCounts &m = frame.scene;
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
         << ",\"world_triangles\":" << c.worldTriangles << ",\"static_models\":{\"in_world\":"
         << m.staticModels << ",\"drawn\":" << m.staticModelsDrawn << ",\"surfaces\":" << m.staticModelSurfaces
         << ",\"draw_calls\":" << m.staticModelDrawCalls << ",\"triangles\":" << m.staticModelTriangles
         << "},\"sun_shadow\":{\"sun\":" << (m.sunLight ? "true" : "false")
         << ",\"sun_used\":" << (m.sunUsed ? "true" : "false") << ",\"shadow_maps\":"
         << (m.shadowMaps ? "true" : "false") << ",\"drawn\":" << (m.sunShadowDrawn ? "true" : "false")
         << ",\"surfaces\":" << m.sunShadowSurfaces << ",\"static_model_surfaces\":"
         << m.sunShadowStaticModelSurfaces << "},\"smc_patches\":" << frame.smcPatches
         << ",\"seconds\":" << frame.seconds << "}";
    return json.str();
}

void SayFrame(const char *label, int draw, const FrameResult &frame)
{
    const wdr::FrameStats &s = frame.stats;
    const wdr::DrawCounts &c = frame.counts;
    const wdr::SceneCounts &m = frame.scene;
    Say("world_draw_frame view=%s draw=%d size=%ux%u colors=%zu dominant=%.3f luma=%.1f+-%.1f detail_tiles=%u "
        "digest=%s lit=%u decal=%u emissive=%u world_calls=%u world_tris=%u smodels=%u/%u smodel_surfs=%u "
        "smodel_calls=%u smodel_tris=%u sun_shadow=%s sun_shadow_surfs=%u/%u smc_patches=%u seconds=%.3f\n",
        label, draw, s.width, s.height, s.distinctColors, s.dominantShare, s.meanLuma, s.lumaStdDev, s.detailTiles,
        Hex64(s.digest).c_str(), c.litSurfaces, c.decalSurfaces, c.emissiveSurfaces, c.worldDrawCalls,
        c.worldTriangles, m.staticModelsDrawn, m.staticModels, m.staticModelSurfaces, m.staticModelDrawCalls,
        m.staticModelTriangles,
        m.sunShadowDrawn  ? "drawn"
        : !m.sunLight    ? "no_sun"
        : !m.sunUsed     ? "sun_unused"
        : !m.shadowMaps  ? "maps_off"
                         : "not_drawn",
        m.sunShadowStaticModelSurfaces, m.sunShadowSurfaces, frame.smcPatches, frame.seconds);
}

} // namespace kisak_world_draw_engine
