// Offline world front-end probe (declared in world_frontend_probe.h).
//
// What runs, in order, and where it comes from:
//
//   1. DB_LoadXAssets for XZoneInfo{map, DB_ZONE_GAME, DB_ZONE_GAME}: the
//      fast-file branch of the server's map load (src/server_mp/sv_init_mp.cpp),
//      the same zone operation the world surface inventory performs. Device
//      objects stay null: R_MakeDedicated latched r_loadForRenderer off before
//      any zone was opened.
//   2. R_SetWorldPtr_FastFile (r_bsp.cpp), the call R_LoadWorld makes to find the
//      world. The rest of R_LoadWorld (lighting dvars, sun, shadow cookies, static
//      model lighting, BSP images, the dynamic entity filter, shadow-caster cells,
//      lightmap and probe textures) prepares lighting and device data that the
//      surface walk below does not read, and is not run.
//   3. The needSortMaterials branch of R_BeginFrame (r_rendercmds.cpp):
//      Material_Sort, then R_SortWorldSurfaces, which writes every world
//      surface's draw-surface key into dpvs.surfaceMaterials and ends by filing
//      each shadow-casting surface under the primary lights that reach it
//      (R_SetPrimaryLightShadowSurfaces, against the zone's COMWORLD lights).
//   4. The scene setup the synthetic front-end test performs (R_InitSceneBuffers,
//      R_InitSceneData, R_InitScene), with worker commands run on this thread and
//      the pre-tessellated path off, then per view: R_UpdateLodParms,
//      R_CorrectLodScale, R_SetViewParmsForScene, R_ClearDpvsScene,
//      R_SetupWorldSurfacesDpvs, R_CellForPoint, R_AddWorldSurfacesDpvs,
//      R_AddAllBspDrawSurfacesCamera, R_AddAllBspDrawSurfacesCameraNonlit for the
//      emissive range and R_MergeAndEmitDrawSurfLists per stage.
//
// Each view runs three frames: the portal walk from the camera cell, the same
// view again (it must give the same frame), and the frustum-only walk (camera
// cell -1), whose visible surfaces must include the portal walk's. Every frame
// is held to world_frontend_invariants.h.
//
// What this does not claim: no device, texture, shader or pixel is involved.
// Static models, entities, effects, shadows and lighting are walked by the
// production code where the surface walk reaches them, but nothing about them is
// checked. The JSON holds counts and the map's own name only: no geometry,
// material names or asset bytes.

#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <qcommon/com_bsp.h>
#include <database/database.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_drawsurf.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_pretess.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_workercmds.h>

#include "world_frontend_invariants.h"
#include "world_frontend_probe.h"
#include "world_surface_export.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// r_add_bsp.cpp defines it without a header declaration; the synthetic test
// declares it the same way.
void R_AddAllBspDrawSurfacesCameraNonlit(uint32_t beginSurface, uint32_t endSurface, uint32_t stage);

namespace
{
namespace wf = kisak_world_frontend;

// dpvsGlob.cellVisibleBits holds 32 words, so the walk cannot mark more cells.
constexpr int kMaxCells = 32 * 32;
// Surface indices are uint16_t throughout the BSP draw path (triSurfList, the
// primitive stream, sortedSurfIndex).
constexpr uint32_t kMaxSurfaces = 65536;
constexpr size_t kMaxViews = 256;
constexpr size_t kMaxReportedProblems = 16;
// The default cg_fov is a 65 degree horizontal field of view at 4:3.
constexpr float kHalfFovXDegrees = 32.5f;
constexpr float kAspectYOverX = 0.75f;
constexpr int kUseCameraCell = -100;

constexpr uint32_t kStageDrawSurfType[wf::kStageCount] = {
    DRAW_SURF_BSP_CAMERA_LIT,
    DRAW_SURF_BSP_CAMERA_DECAL,
    DRAW_SURF_BSP_CAMERA_EMISSIVE,
};

void Require(bool ok, const std::string &why)
{
    if (!ok)
        throw std::runtime_error(why);
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
    Require(fd >= 0, "cannot create " + file.string() + ": " + std::strerror(errno));
    size_t written = 0;
    while (written < text.size())
    {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            ::close(fd);
            throw std::runtime_error("cannot write " + file.string());
        }
        written += size_t(n);
    }
    Require(::close(fd) == 0, "cannot close " + file.string());
}

template <class Header> struct Collected
{
    std::array<Header *, 4> items{};
    size_t count = 0;
    bool overflow = false;
};

void CollectWorld(XAssetHeader header, void *context)
{
    auto &found = *static_cast<Collected<GfxWorld> *>(context);
    if (found.count == found.items.size())
        found.overflow = true;
    else
        found.items[found.count++] = header.gfxWorld;
}

void CollectMapEnts(XAssetHeader header, void *context)
{
    auto &found = *static_cast<Collected<MapEnts> *>(context);
    if (found.count == found.items.size())
        found.overflow = true;
    else
        found.items[found.count++] = header.mapEnts;
}

// The one packaged world in the zone, found by enumeration so a default asset the
// database synthesizes for a bad name cannot stand in for it.
GfxWorld *FindPackagedWorld(const char *mapName)
{
    Collected<GfxWorld> worlds;
    DB_EnumXAssets(ASSET_TYPE_GFXWORLD, CollectWorld, &worlds, false);
    Require(!worlds.overflow, "GFXWORLD asset count exceeds bound");
    GfxWorld *world = nullptr;
    unsigned packaged = 0;
    for (size_t i = 0; i < worlds.count; ++i)
    {
        GfxWorld *candidate = worlds.items[i];
        if (!candidate || !candidate->name || DB_IsXAssetDefault(ASSET_TYPE_GFXWORLD, candidate->name))
            continue;
        ++packaged;
        world = candidate;
    }
    Require(packaged == 1, "expected exactly one packaged GFXWORLD in the map zone, found " + std::to_string(packaged));
    const std::string name = world->name;
    const std::string baseName = world->baseName ? world->baseName : "";
    Require(name.find(mapName) != std::string::npos || baseName.find(mapName) != std::string::npos,
            "loaded GFXWORLD '" + name + "' does not name the requested map '" + mapName + "'");
    return world;
}

const MapEnts *FindMapEnts(const char *worldName)
{
    Collected<MapEnts> ents;
    DB_EnumXAssets(ASSET_TYPE_MAP_ENTS, CollectMapEnts, &ents, false);
    Require(!ents.overflow, "MAP_ENTS asset count exceeds bound");
    const MapEnts *found = nullptr;
    for (size_t i = 0; i < ents.count; ++i)
    {
        const MapEnts *candidate = ents.items[i];
        if (!candidate || !candidate->name || std::strcmp(candidate->name, worldName))
            continue;
        Require(!found, std::string("more than one MAP_ENTS asset is named ") + worldName);
        found = candidate;
    }
    Require(found != nullptr, std::string("no MAP_ENTS asset is named ") + worldName);
    Require(found->entityString != nullptr && found->numEntityChars > 0, "the map's entity string is empty");
    return found;
}

// Everything the checks read from the world, with each array bounded before use.
wf::WorldShape ReadWorldShape(const GfxWorld &world)
{
    Require(world.modelCount >= 1 && world.models != nullptr, "the world has no brush model 0");
    const GfxBrushModel &worldModel = world.models[0];
    // R_SortWorldSurfaces asserts the same thing before it walks model 0.
    Require(worldModel.startSurfIndex == 0, "world model 0 does not start at surface 0");
    const GfxWorldDpvsStatic &dpvs = world.dpvs;
    // surfaceCount is a uint16_t, so kMaxSurfaces bounds it by type.
    static_assert(sizeof(worldModel.surfaceCount) == 2 && kMaxSurfaces == 65536);
    Require(worldModel.surfaceCount > 0, "world model 0 has no surfaces");
    Require(worldModel.surfaceCount <= dpvs.surfaceVisDataCount,
            "world model 0 has more surfaces than the visibility arrays hold");
    Require(worldModel.surfaceCount <= dpvs.staticSurfaceCount,
            "world model 0 has more surfaces than the static surface table");
    Require(dpvs.surfaces && dpvs.surfaceMaterials && dpvs.sortedSurfIndex && dpvs.surfaceVisData[0],
            "a world surface table is absent");
    const int cellCount = world.dpvsPlanes.cellCount;
    Require(cellCount > 0 && cellCount <= kMaxCells && world.cells != nullptr,
            "world cell count " + std::to_string(cellCount) + " is out of range");

    wf::WorldShape shape;
    shape.surfaceCount = worldModel.surfaceCount;
    shape.stageRange[wf::kStageLit] = {dpvs.litSurfsBegin, dpvs.litSurfsEnd};
    shape.stageRange[wf::kStageDecal] = {dpvs.decalSurfsBegin, dpvs.decalSurfsEnd};
    shape.stageRange[wf::kStageEmissive] = {dpvs.emissiveSurfsBegin, dpvs.emissiveSurfsEnd};
    shape.surfaceKey.resize(shape.surfaceCount);
    for (uint32_t s = 0; s < shape.surfaceCount; ++s)
        shape.surfaceKey[s] = dpvs.surfaceMaterials[s].packed;

    const uint32_t sortedCount = dpvs.staticSurfaceCount;
    auto addRange = [&](std::vector<uint32_t> &out, uint32_t first, uint32_t count, const std::string &what) {
        Require(uint64_t(first) + count <= sortedCount,
                what + " lists sorted surfaces " + std::to_string(first) + "+" + std::to_string(count) + " of " +
                    std::to_string(sortedCount));
        for (uint32_t k = 0; k < count; ++k)
            out.push_back(dpvs.sortedSurfIndex[first + k]);
    };
    shape.cellSurfaces.resize(size_t(cellCount));
    for (int c = 0; c < cellCount; ++c)
    {
        const GfxCell &cell = world.cells[c];
        std::vector<uint32_t> &surfaces = shape.cellSurfaces[size_t(c)];
        const std::string where = "cell " + std::to_string(c);
        Require(cell.aabbTreeCount >= 0 && (cell.aabbTreeCount == 0 || cell.aabbTree != nullptr),
                where + " has an invalid AABB tree table");
        for (int t = 0; t < cell.aabbTreeCount; ++t)
            addRange(surfaces, cell.aabbTree[t].startSurfIndex, cell.aabbTree[t].surfaceCount,
                     where + " AABB node " + std::to_string(t));
        Require(cell.cullGroupCount >= 0 && (cell.cullGroupCount == 0 || cell.cullGroups != nullptr),
                where + " has an invalid cull group table");
        for (int g = 0; g < cell.cullGroupCount; ++g)
        {
            const int group = cell.cullGroups[g];
            Require(group >= 0 && group < world.cullGroupCount && dpvs.cullGroups != nullptr,
                    where + " names cull group " + std::to_string(group) + " of " +
                        std::to_string(world.cullGroupCount));
            Require(dpvs.cullGroups[group].startSurfIndex >= 0 && dpvs.cullGroups[group].surfaceCount >= 0,
                    "cull group " + std::to_string(group) + " has a negative range");
            addRange(surfaces, uint32_t(dpvs.cullGroups[group].startSurfIndex),
                     uint32_t(dpvs.cullGroups[group].surfaceCount), "cull group " + std::to_string(group));
        }
        std::sort(surfaces.begin(), surfaces.end());
        surfaces.erase(std::unique(surfaces.begin(), surfaces.end()), surfaces.end());
    }
    return shape;
}

// ---------------------------------------------------------------------------
// Frame driver
// ---------------------------------------------------------------------------

struct View
{
    std::unique_ptr<refdef_s> refdef; // 16 KiB, zeroed
    GfxViewParms viewParms;
};

// A view described the way a client describes one: a refdef with an eye, an
// axis and the field of view. The renderer's own functions derive the LOD
// parameters and the view parameters from it.
void SetupView(const float *eye, const float (*axis)[3], View &view)
{
    view.refdef = std::make_unique<refdef_s>();
    refdef_s &rd = *view.refdef;
    for (int i = 0; i < 3; ++i)
    {
        rd.vieworg[i] = eye[i];
        for (int j = 0; j < 3; ++j)
            rd.viewaxis[i][j] = axis[i][j];
    }
    rd.tanHalfFovX = std::tan(kHalfFovXDegrees * float(M_PI) / 180.0f);
    rd.tanHalfFovY = rd.tanHalfFovX * kAspectYOverX;
    R_UpdateLodParms(&rd, &rg.lodParms);
    R_CorrectLodScale(&rd);
    R_SetViewParmsForScene(&rd, &view.viewParms);
}

// The BSP draw path's primitive stream: repeated {count, ceil(count / 2) words of
// packed uint16 surface indices}, ended by a zero count (R_ReadBspDrawSurfs).
bool ReadBatch(const GfxBackEndData &data, const GfxDrawSurf &drawSurf, wf::Batch &out, std::string &error)
{
    out.key = drawSurf.packed;
    out.surfaces.clear();
    constexpr uint32_t kWords = sizeof(data.primDrawSurfsBuf) / sizeof(data.primDrawSurfsBuf[0]);
    uint32_t pos = drawSurf.fields.objectId;
    while (pos < kWords)
    {
        const uint32_t count = data.primDrawSurfsBuf[pos++];
        if (count == 0)
            return true;
        if (pos + ((count + 1) >> 1) > kWords)
            break;
        const auto *list = reinterpret_cast<const uint16_t *>(&data.primDrawSurfsBuf[pos]);
        out.surfaces.insert(out.surfaces.end(), list, list + count);
        pos += (count + 1) >> 1;
    }
    error = "a primitive stream runs past the buffer";
    return false;
}

struct FrameRun
{
    wf::Frame frame;
    std::string error;
};

FrameRun RunFrame(const GfxWorld &world, const wf::WorldShape &shape, GfxBackEndData &data, const View &view,
                  int forcedCell)
{
    FrameRun run;
    wf::Frame &frame = run.frame;
    std::memset(&data, 0, sizeof(data));
    frontEndDataOut = &data;
    for (volatile long &count : scene.drawSurfCount)
        count = 0;

    R_ClearDpvsScene();
    R_SetupWorldSurfacesDpvs(&view.viewParms);
    frame.cameraCell = R_CellForPoint(&world, view.viewParms.origin);
    R_AddWorldSurfacesDpvs(&view.viewParms, forcedCell == kUseCameraCell ? frame.cameraCell : forcedCell);
    R_WaitWorkerCmdsOfType(WRKCMD_DPVS_CELL_STATIC);

    const uint8_t *vis = world.dpvs.surfaceVisData[0];
    for (uint32_t s = 0; s < world.dpvs.surfaceVisDataCount; ++s)
        if (vis[s])
            frame.visibleSurfaces.push_back(s);
    for (int c = 0; c < int(shape.cellSurfaces.size()); ++c)
        if (dpvsGlob.cellVisibleBits[c >> 5] & (1u << (c & 31)))
            frame.visibleCells.push_back(c);

    R_AddAllBspDrawSurfacesCamera();
    R_AddAllBspDrawSurfacesCameraNonlit(world.dpvs.emissiveSurfsBegin, world.dpvs.emissiveSurfsEnd,
                                        DRAW_SURF_BSP_CAMERA_EMISSIVE);
    for (unsigned stage = 0; stage < wf::kStageCount; ++stage)
    {
        const uint32_t type = kStageDrawSurfType[stage];
        // A full list means R_AddBspDrawSurfs may have stopped adding; that frame
        // cannot be held to the completeness rule, so it is a failure, not a pass.
        if (scene.drawSurfCount[type] >= scene.maxDrawSurfCount[type])
        {
            run.error = std::string(wf::StageName(stage)) + " draw-surface list is full (" +
                        std::to_string(scene.maxDrawSurfCount[type]) + ")";
            return run;
        }
        const int first = data.drawSurfCount;
        R_MergeAndEmitDrawSurfLists(DrawSurfType(type), 1);
        if (data.drawSurfCount - first != int(scene.drawSurfCount[type]))
        {
            run.error = std::string(wf::StageName(stage)) + " stage emitted " +
                        std::to_string(data.drawSurfCount - first) + " of " +
                        std::to_string(scene.drawSurfCount[type]) + " draw surfaces";
            return run;
        }
        for (int i = first; i < data.drawSurfCount; ++i)
        {
            wf::Batch batch;
            if (!ReadBatch(data, data.drawSurfs[i], batch, run.error))
                return run;
            frame.batches[stage].push_back(std::move(batch));
        }
    }
    return run;
}

void SetBool(const dvar_s *dvar, bool value)
{
    Dvar_SetBool(const_cast<dvar_s *>(dvar), value);
}

// A frame from a walk that starts in no cell (camera cell -1): every cell is
// tested against the frustum, so the surface and batch rules apply with every
// cell counted as visited, and the camera-cell rules do not.
wf::Frame AllCellsVisited(const wf::WorldShape &shape, const wf::Frame &frame)
{
    wf::Frame relaxed = frame;
    relaxed.visibleCells.clear();
    for (int c = 0; c < int(shape.cellSurfaces.size()); ++c)
        relaxed.visibleCells.push_back(c);
    relaxed.cameraCell = relaxed.visibleCells.empty() ? -1 : 0;
    return relaxed;
}

struct ViewResult
{
    std::string classname;
    int cameraCell = -1;
    size_t visibleCells = 0;
    size_t visibleSurfaces = 0;
    size_t frustumOnlySurfaces = 0;
    size_t visibleOutsideStages = 0;
    size_t batches[wf::kStageCount] = {};
    size_t emitted[wf::kStageCount] = {};
    size_t keyDescents[wf::kStageCount] = {};
    std::vector<std::string> problems;
};

ViewResult ProbeView(const GfxWorld &world, const wf::WorldShape &shape, GfxBackEndData &data,
                     const wf::SpawnView &spawn)
{
    ViewResult result;
    result.classname = spawn.classname;
    float axis[3][3];
    AnglesToAxis(spawn.angles, axis);
    View view;
    SetupView(spawn.eye, axis, view);

    FrameRun portal = RunFrame(world, shape, data, view, kUseCameraCell);
    if (!portal.error.empty())
    {
        result.problems.push_back("portal walk: " + portal.error);
        return result;
    }
    FrameRun again = RunFrame(world, shape, data, view, kUseCameraCell);
    FrameRun frustum = RunFrame(world, shape, data, view, -1);
    if (!again.error.empty())
        result.problems.push_back("repeated portal walk: " + again.error);
    if (!frustum.error.empty())
        result.problems.push_back("frustum-only walk: " + frustum.error);

    const wf::Frame &frame = portal.frame;
    result.cameraCell = frame.cameraCell;
    // An eye in no cell makes R_AddWorldSurfacesDpvs walk by frustum alone, which
    // the engine does on purpose. A player spawn is where a player stands, so its
    // eye must be in a cell; the intermission camera may hang outside the level.
    const bool intermission = spawn.classname == "mp_global_intermission";
    const bool outsideCells = frame.cameraCell < 0;
    result.visibleCells = frame.visibleCells.size();
    result.visibleSurfaces = frame.visibleSurfaces.size();
    result.frustumOnlySurfaces = frustum.frame.visibleSurfaces.size();
    result.visibleOutsideStages = wf::CountVisibleOutsideStages(shape, frame);
    for (unsigned stage = 0; stage < wf::kStageCount; ++stage)
    {
        result.batches[stage] = frame.batches[stage].size();
        for (const wf::Batch &batch : frame.batches[stage])
            result.emitted[stage] += batch.surfaces.size();
        result.keyDescents[stage] = wf::CountKeyDescents(frame.batches[stage]);
    }

    if (outsideCells && !intermission)
        result.problems.push_back("the spawn eye is in no cell");
    for (const std::string &line :
         wf::CheckFrame(shape, outsideCells ? AllCellsVisited(shape, frame) : frame, kMaxReportedProblems))
        result.problems.push_back(line);
    if (again.error.empty() && !wf::SameFrame(frame, again.frame))
        result.problems.push_back("the same view run twice gave different frames");
    if (frustum.error.empty())
    {
        if (!wf::IsSubset(frame.visibleSurfaces, frustum.frame.visibleSurfaces))
            result.problems.push_back("the portal walk sees a surface the frustum-only walk does not");
        // The frustum-only walk starts from no cell and tests every cell against
        // the frustum, so it is held to the surface and batch rules with every
        // cell counted as visited and the camera-cell rules taken out.
        const wf::Frame relaxed = AllCellsVisited(shape, frustum.frame);
        for (const std::string &line : wf::CheckFrame(shape, relaxed, kMaxReportedProblems))
            result.problems.push_back("frustum-only walk: " + line);
    }
    return result;
}
} // namespace

void Kisak_ProbeWorldFrontEnd(const std::filesystem::path &directory, const char *mapName,
                              const wf::Layout &loaderLayout)
{
    Require(kisak_world_inventory::IsAllowedMapName(mapName), "KISAK_WORLD_FRONTEND_MAP must name an allowlisted map");
    // The loader and the front end are compiled with different definitions; they
    // must agree on every struct they hand each other before anything is loaded.
    const wf::Layout frontEndLayout = KISAK_WORLD_FRONTEND_LAYOUT();
    const size_t differs = wf::FirstLayoutDifference(loaderLayout, frontEndLayout);
    Require(differs == wf::kLayoutTypeCount,
            differs < wf::kLayoutTypeCount
                ? std::string("struct ") + wf::kLayoutTypeNames[differs] + " is " +
                      std::to_string(loaderLayout.size[differs]) + " bytes to the loader and " +
                      std::to_string(frontEndLayout.size[differs]) + " to the front end"
                : std::string());
    std::printf("world_frontend_layout=agreed types=%zu\n", wf::kLayoutTypeCount);
    const auto started = std::chrono::steady_clock::now();

    // The zone loader must keep geometry in CPU memory: no device exists here.
    Require(r_loadForRenderer && !r_loadForRenderer->current.enabled,
            "r_loadForRenderer must be latched off before a zone is loaded");
    Require(dx.device == nullptr, "a D3D device exists in the offline probe");

    // 1. Zone load.
    XZoneInfo zone{mapName, DB_ZONE_GAME, DB_ZONE_GAME};
    std::printf("world_frontend_zone=%s alloc=%d free=%d\n", mapName, DB_ZONE_GAME, DB_ZONE_GAME);
    DB_LoadXAssets(&zone, 1, 1);
    DB_SyncXAssets();
    GfxWorld *packaged = FindPackagedWorld(mapName);

    // 2. The renderer's own world lookup must land on the same asset.
    Require(rgp.world == nullptr, "a world was registered before the probe");
    R_SetWorldPtr_FastFile(packaged->name);
    Require(rgp.world == packaged, "R_SetWorldPtr_FastFile did not find the packaged world");
    Require(rgp.needSortMaterials != 0, "R_SetWorldPtr_FastFile did not request a material sort");
    const GfxWorld &world = *rgp.world;
    // GFXWORLD and COMWORLD are singletons the zone loads into (s_world, comWorld).
    // R_SortWorldSurfaces ends in R_SetPrimaryLightShadowSurfaces, which culls every
    // shadow-casting surface against the primary lights in comWorld and reads the
    // light regions through s_world. The server's map load checks the same asset
    // (Com_LoadWorld_FastFile); here it is matched by name rather than looked up,
    // so a missing asset fails instead of waiting on the database. Bound that light
    // loop before it runs.
    Require(&world == &s_world, "the packaged world is not the renderer's world singleton");
    Require(comWorld.isInUse && comWorld.name && !std::strcmp(comWorld.name, world.name),
            std::string("the map zone did not load a COMWORLD named like its GFXWORLD '") + world.name + "' (" +
                (comWorld.isInUse ? "in use" : "not in use") + ", named '" +
                (comWorld.name ? comWorld.name : "") + "')");
    Require(world.primaryLightCount <= comWorld.primaryLightCount,
            "the world has " + std::to_string(world.primaryLightCount) + " primary lights, COMWORLD " +
                std::to_string(comWorld.primaryLightCount));
    Require(world.primaryLightCount == 0 || (world.shadowGeom && world.lightRegion),
            "the world's per-light shadow or region tables are absent");

    // 3. The first frame's material and world-surface sort.
    rgp.needSortMaterials = 0;
    Material_Sort();
    R_SortWorldSurfaces();
    Require(rgp.materialCount > 0, "Material_Sort found no materials");

    const wf::WorldShape shape = ReadWorldShape(world);
    const std::vector<std::string> shapeProblems = wf::CheckWorldShape(shape);
    if (!shapeProblems.empty())
        throw std::runtime_error("world cannot be checked: " + shapeProblems.front());
    size_t distinctKeys = 0;
    {
        std::vector<uint64_t> keys = shape.surfaceKey;
        for (uint64_t &key : keys)
            key = wf::KeyWithoutObjectId(key);
        std::sort(keys.begin(), keys.end());
        distinctKeys = size_t(std::unique(keys.begin(), keys.end()) - keys.begin());
    }

    // 4. Scene setup.
    SetBool(r_smp_worker, false); // front-end work runs on this thread
    SetBool(r_pretess, false);    // the pre-tessellated path writes GPU index buffers
    Require(!r_smp_worker->current.enabled && !r_pretess->current.enabled,
            "r_smp_worker and r_pretess did not switch off");
    rg.drawWorld = r_drawWorld->current.enabled;
    rg.drawSModels = r_drawSModels->current.enabled;
    rg.drawBModels = r_drawBModels->current.enabled;
    rg.drawXModels = r_drawXModels->current.enabled;
    Require(rg.drawWorld, "r_drawWorld is off");
    auto data = std::make_unique<GfxBackEndData>();
    frontEndDataOut = data.get();
    R_InitSceneBuffers();
    R_InitSceneData(0);
    R_InitScene();

    const MapEnts *ents = FindMapEnts(world.name);
    const wf::SpawnScan scan = wf::ScanSpawnViews(ents->entityString, size_t(ents->numEntityChars), kMaxViews);
    Require(scan.error.empty(), "entity string: " + scan.error);
    Require(!scan.views.empty(), "the map has no player spawn or intermission camera");

    std::vector<ViewResult> results;
    size_t failedViews = 0;
    for (const wf::SpawnView &spawn : scan.views)
    {
        results.push_back(ProbeView(world, shape, *data, spawn));
        failedViews += !results.back().problems.empty();
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // Totals over all views.
    size_t cellsSeen = 0, surfacesSeen = 0;
    {
        std::vector<char> anyCell(shape.cellSurfaces.size(), 0);
        for (const ViewResult &r : results)
            if (r.cameraCell >= 0 && size_t(r.cameraCell) < anyCell.size())
                anyCell[size_t(r.cameraCell)] = 1;
        for (char c : anyCell)
            cellsSeen += c;
    }
    size_t maxVisible = 0, minVisible = SIZE_MAX, descentViews = 0, outsideViews = 0, outsideCellViews = 0;
    size_t frustumSeen = 0;
    size_t batchesSeen[wf::kStageCount] = {};
    for (const ViewResult &r : results)
    {
        maxVisible = std::max(maxVisible, r.visibleSurfaces);
        minVisible = std::min(minVisible, r.visibleSurfaces);
        surfacesSeen += r.visibleSurfaces;
        frustumSeen += r.frustumOnlySurfaces;
        for (unsigned stage = 0; stage < wf::kStageCount; ++stage)
            batchesSeen[stage] += r.batches[stage];
        descentViews += (r.keyDescents[0] + r.keyDescents[1] + r.keyDescents[2]) != 0;
        outsideViews += r.visibleOutsideStages != 0;
        outsideCellViews += r.cameraCell < 0;
    }
    const double viewCount = double(results.size());

    std::ostringstream json;
    json << "{\"schema\":1,\"tool\":\"world_frontend_probe\",\"map\":" << Json(mapName)
         << ",\"world\":" << Json(world.name) << ",\"surfaces\":" << shape.surfaceCount
         << ",\"cells\":" << shape.cellSurfaces.size() << ",\"cull_groups\":" << world.cullGroupCount
         << ",\"static_models\":" << world.dpvs.smodelCount << ",\"materials_sorted\":" << rgp.materialCount
         << ",\"distinct_surface_keys\":" << distinctKeys << ",\"stage_ranges\":{";
    for (unsigned stage = 0; stage < wf::kStageCount; ++stage)
        json << (stage ? "," : "") << Json(wf::StageName(stage)) << ":[" << shape.stageRange[stage].begin << ","
             << shape.stageRange[stage].end << "]";
    json << "},\"view_rule\":{\"spawn_classes\":\"mp_*_spawn*\",\"spawn_eye_height\":" << wf::kSpawnEyeHeight
         << ",\"intermission\":\"mp_global_intermission\",\"half_fov_x_degrees\":" << kHalfFovXDegrees
         << ",\"aspect_y_over_x\":" << kAspectYOverX << "},\"entities\":" << scan.entities
         << ",\"matching_entities\":" << scan.matching << ",\"views\":[";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const ViewResult &r = results[i];
        json << (i ? "," : "") << "\n{\"index\":" << i << ",\"class\":" << Json(r.classname)
             << ",\"camera_cell\":" << r.cameraCell << ",\"visible_cells\":" << r.visibleCells
             << ",\"visible_surfaces\":" << r.visibleSurfaces << ",\"frustum_only_surfaces\":"
             << r.frustumOnlySurfaces << ",\"visible_outside_stages\":" << r.visibleOutsideStages;
        for (unsigned stage = 0; stage < wf::kStageCount; ++stage)
            json << ",\"" << wf::StageName(stage) << "\":{\"batches\":" << r.batches[stage]
                 << ",\"surfaces\":" << r.emitted[stage] << ",\"key_descents\":" << r.keyDescents[stage] << "}";
        json << ",\"problems\":[";
        for (size_t p = 0; p < r.problems.size(); ++p)
            json << (p ? "," : "") << Json(r.problems[p]);
        json << "]}";
    }
    json << "],\"views_outside_cells\":" << outsideCellViews << ",\"failed_views\":" << failedViews
         << ",\"seconds\":" << seconds << "}\n";
    WriteExclusiveFile(directory / "world-frontend-probe.json", json.str());

    std::printf("world_frontend_world=%s surfaces=%u cells=%zu cull_groups=%d static_models=%u materials_sorted=%d "
                "distinct_surface_keys=%zu\n",
                world.name, shape.surfaceCount, shape.cellSurfaces.size(), world.cullGroupCount,
                world.dpvs.smodelCount, rgp.materialCount, distinctKeys);
    std::printf("world_frontend_views=%zu matching_entities=%zu camera_cells_distinct=%zu views_outside_cells=%zu "
                "visible_min=%zu visible_max=%zu visible_mean=%.1f frustum_only_mean=%.1f\n",
                results.size(), scan.matching, cellsSeen, outsideCellViews, minVisible, maxVisible,
                double(surfacesSeen) / viewCount, double(frustumSeen) / viewCount);
    std::printf("world_frontend_batches_mean lit=%.1f decal=%.1f emissive=%.1f views_with_key_descents=%zu "
                "views_with_unstaged_visible=%zu seconds=%.2f\n",
                double(batchesSeen[wf::kStageLit]) / viewCount, double(batchesSeen[wf::kStageDecal]) / viewCount,
                double(batchesSeen[wf::kStageEmissive]) / viewCount, descentViews, outsideViews, seconds);
    // Every problem is in the JSON; the console gets the first few.
    constexpr size_t kMaxPrintedProblems = 48;
    size_t printed = 0, problemCount = 0;
    for (size_t i = 0; i < results.size(); ++i)
        for (const std::string &line : results[i].problems)
            if (++problemCount <= kMaxPrintedProblems)
                std::printf("world_frontend_problem view=%zu class=%s %s\n", i, results[i].classname.c_str(),
                            line.c_str()),
                    ++printed;
    if (problemCount > printed)
        std::printf("world_frontend_problem omitted=%zu (all are in world-frontend-probe.json)\n",
                    problemCount - printed);
    std::printf("world_frontend_probe=%s failed_views=%zu of %zu\n", failedViews ? "failed" : "passed", failedViews,
                results.size());
    std::fflush(stdout);
    Require(failedViews == 0, std::to_string(failedViews) + " of " + std::to_string(results.size()) +
                                  " views broke a front-end rule (see world_frontend_problem lines)");
}
