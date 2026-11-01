// Asset-free test of the renderer's world front end on native i386 Linux.
//
// It runs the production code that decides what part of a world is drawn and
// how it is batched -- the same functions R_GenerateSortedDrawSurfs calls --
// against a small synthetic GfxWorld, and checks the result against
// expectations derived independently from the geometry:
//
//   R_InitSceneBuffers / R_InitSceneData / R_ClearDpvsScene   (r_dpvs.cpp)
//   R_InitScene, R_SetViewParmsForScene                        (r_scene.cpp)
//   R_SetupWorldSurfacesDpvs, R_CellForPoint                   (r_dpvs.cpp)
//   R_AddWorldSurfacesDpvs: portal walk, frustum and portal
//     clipping of each cell's AABB tree and cull groups        (r_dpvs.cpp, r_dpvs_static.cpp)
//   R_AddAllBspDrawSurfacesCamera, the emissive range           (r_add_bsp.cpp, r_add_cmdbuf.cpp)
//   R_MergeAndEmitDrawSurfLists                                (r_pretess.cpp)
//
// What the world looks like (units are engine units, +x is forward):
//
//   cell 0: x in [0, 512]      cell 1: x in [512, 1024]     y in [-256, 256], z in [0, 256]
//   the two cells meet at x = 512 through one portal, a window y in [-64, 64], z in [64, 192]
//   the camera stands in cell 0 at (64, 0, 128) and looks down +x with a 90 degree field of view
//
// Surfaces are stored in material order, which is how the loader sorts them:
//
//   surface  material   where                                   expected (first view)
//   0        A          cell 0, ahead                           visible
//   1        A          cell 0, ahead                           visible
//   2        A          cell 0, behind the camera               culled by the near plane
//   3        A          cell 0, ahead, 416..436 units away      visible; culled once r_zfar < 416
//   4        A          cell 1, seen through the portal          visible
//   5        B          cell 0, ahead                           visible
//   6        B          cell 1, seen through the portal          visible
//   7        B          cell 1, in the camera frustum but
//                       outside the portal window               culled by the portal planes
//   8        C decal    cell 0, ahead                           visible (decal stage)
//   9        D emissive cell 0, ahead, only in a cull group     visible (emissive range)
//
// So the lit stage must hold exactly two draw surfaces: material A over surfaces
// {0, 1, 3, 4} -- one batch that spans both cells and skips the culled surface 2
// -- and material B over {5, 6}. The decal stage holds C over {8} and the
// emissive range D over {9}. Further views check a far plane, the walk from
// cell 1 back through the portal, the "no camera cell" path that tests every cell
// against the frustum alone, and the r_skipPvs and r_drawDecals switches.
//
// What this does NOT claim: no device, texture, shader or pixel is involved. The
// pre-tessellated path (r_pretess) needs GPU index buffers and is switched off
// here; static models, entities, effects and shadows are absent. r_scene.cpp
// calls the D3D9 API, so the runner compiles it through the native D3D9
// boundary (DXVK Native's headers, no DXVK library); no D3D9 method runs.

#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_drawsurf.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_pretess.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_workercmds.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <script/scr_stringlist.h>
#include <universal/com_math.h>
#include <qcommon/qcommon.h>
#include <linux/sys_critical.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

void R_AddAllBspDrawSurfacesCameraNonlit(uint32_t beginSurface, uint32_t endSurface, uint32_t stage);

namespace
{
int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    ++g_checks;
    if (!ok)
        ++g_failures;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

// ---------------------------------------------------------------------------
// Synthetic world
// ---------------------------------------------------------------------------

constexpr int kCellCount = 2;
constexpr int kSurfaceCount = 10;
constexpr uint32_t kLitBegin = 0;
constexpr uint32_t kLitEnd = 8;    // surfaces 0..7
constexpr uint32_t kDecalBegin = 8;
constexpr uint32_t kDecalEnd = 9;  // surface 8
constexpr uint32_t kEmissiveBegin = 9;
constexpr uint32_t kEmissiveEnd = 10; // surface 9

struct Box
{
    float mins[3];
    float maxs[3];
};

// Material sort keys, as R_SortMaterials would have assigned them. They differ in
// materialSortedIndex only, so the lit pair shares one primary sort key.
GfxDrawSurf MakeKey(uint32_t sortedIndex, uint32_t primarySortKey)
{
    GfxDrawSurf key;
    key.packed = 0;
    key.fields.materialSortedIndex = sortedIndex;
    key.fields.primarySortKey = primarySortKey;
    key.fields.surfType = SF_TRIANGLES;
    key.fields.primaryLightIndex = 0;
    key.fields.reflectionProbeIndex = 0;
    return key;
}

struct SyntheticWorld
{
    GfxWorld world{};
    GfxCell cells[kCellCount]{};
    GfxPortal portals[2]{}; // [0] in cell 0 -> cell 1, [1] in cell 1 -> cell 0
    float portalVerts[2][4][3]{};
    GfxAabbTree trees[kCellCount]{};
    GfxSurface surfaces[kSurfaceCount]{};
    uint16_t sortedSurfIndex[kSurfaceCount + 1]{};
    GfxDrawSurf surfaceMaterials[kSurfaceCount]{};
    uint32_t surfaceCastsSunShadow[1]{};
    GfxCullGroup cullGroups[1]{};
    int cell0CullGroups[1]{};
    uint8_t surfaceVis[3][kSurfaceCount + 4]{};
    uint8_t smodelVis[3][4]{};
    uint8_t dynEntVis[2][3][4]{};
    uint32_t dynEntCellBits[2][kCellCount]{};
    cplane_s planes[1]{};
    uint16_t nodes[4]{};
    std::unique_ptr<uint32_t[]> sceneEntCellBits;
    GfxBrushModel models[1]{};
    GfxLight sunLight{};
    GfxSceneDynModel *sceneDynModel = nullptr;
    GfxSceneDynBrush *sceneDynBrush = nullptr;
    uint16_t indices[3]{};
};

void SetBox(float (*bounds)[3], const Box &box)
{
    for (int i = 0; i < 3; ++i)
    {
        bounds[0][i] = box.mins[i];
        bounds[1][i] = box.maxs[i];
    }
}

void SetPortal(GfxPortal &portal, float (*verts)[3], float x, float normalSign, GfxCell *target)
{
    // A window y in [-64, 64], z in [64, 192] on the plane x = 512.
    const float corners[4][2] = {{-64, 64}, {64, 64}, {64, 192}, {-64, 192}};
    for (int i = 0; i < 4; ++i)
    {
        verts[i][0] = x;
        verts[i][1] = corners[i][0];
        verts[i][2] = corners[i][1];
    }
    // The portal plane faces away from the cell that owns the portal, as
    // R_LoadPortals stores it: dot(plane, eye) <= 0 for an eye in that cell.
    cplane_s plane{};
    plane.normal[0] = normalSign;
    plane.dist = normalSign * x;
    portal.plane.coeffs[0] = plane.normal[0];
    portal.plane.coeffs[1] = 0.0f;
    portal.plane.coeffs[2] = 0.0f;
    portal.plane.coeffs[3] = -plane.dist;
    R_SetDpvsPlaneSides(&portal.plane);
    portal.cell = target;
    portal.vertices = verts;
    portal.vertexCount = 4;
    // The loader's hull frame: PerpendicularVector, then the cross product.
    PerpendicularVector(plane.normal, portal.hullAxis[0]);
    Vec3Cross(plane.normal, portal.hullAxis[0], portal.hullAxis[1]);
}

std::unique_ptr<SyntheticWorld> BuildWorld()
{
    auto sw = std::make_unique<SyntheticWorld>();
    GfxWorld &w = sw->world;
    w.name = "synthetic_frontend";
    w.baseName = "synthetic_frontend";

    // Surfaces, in sorted (material) order. Bounds are what DPVS tests.
    const Box boxes[kSurfaceCount] = {
        {{200, -40, 100}, {220, -10, 140}},   // 0 A cell 0 ahead
        {{300, 10, 100}, {320, 40, 140}},     // 1 A cell 0 ahead
        {{10, -20, 100}, {40, 20, 140}},      // 2 A cell 0 behind the eye
        {{480, -10, 120}, {500, 10, 140}},    // 3 A cell 0 ahead, near the far planes tested
        {{900, -30, 110}, {920, 30, 130}},    // 4 A cell 1 through the portal
        {{250, -100, 50}, {270, -60, 90}},    // 5 B cell 0 ahead
        {{800, -20, 100}, {820, 20, 150}},    // 6 B cell 1 through the portal
        {{800, 180, 200}, {820, 240, 250}},   // 7 B cell 1 outside the portal window
        {{350, -10, 120}, {352, 10, 140}},    // 8 C decal cell 0 ahead
        {{400, -50, 60}, {420, 50, 80}},      // 9 D emissive cell 0 ahead (cull group only)
    };
    for (int i = 0; i < kSurfaceCount; ++i)
    {
        SetBox(sw->surfaces[i].bounds, boxes[i]);
        sw->surfaces[i].tris.triCount = 1;
        sw->surfaces[i].tris.vertexCount = 3;
    }

    const GfxDrawSurf keyA = MakeKey(5, 2);
    const GfxDrawSurf keyB = MakeKey(9, 2);
    const GfxDrawSurf keyC = MakeKey(3, 10);
    const GfxDrawSurf keyD = MakeKey(12, 20);
    const GfxDrawSurf keys[kSurfaceCount] = {keyA, keyA, keyA, keyA, keyA, keyB, keyB, keyB, keyC, keyD};
    for (int i = 0; i < kSurfaceCount; ++i)
        sw->surfaceMaterials[i] = keys[i];

    // Each cell has one leaf AABB tree over sortedSurfIndex. Decals are counted
    // in surfaceCount and left out of surfaceCountNoDecal, as the loader does.
    // cell 0: surfaces 0 1 2 3 5 then decal 8   cell 1: 4 6 7   cull group: 9
    const uint16_t order[] = {0, 1, 2, 3, 5, 8, 4, 6, 7, 9};
    std::memcpy(sw->sortedSurfIndex, order, sizeof(order));
    GfxAabbTree &tree0 = sw->trees[0];
    tree0.mins[0] = 0, tree0.mins[1] = -256, tree0.mins[2] = 0;
    tree0.maxs[0] = 512, tree0.maxs[1] = 256, tree0.maxs[2] = 256;
    tree0.startSurfIndex = 0, tree0.surfaceCount = 6;
    tree0.startSurfIndexNoDecal = 0, tree0.surfaceCountNoDecal = 5;
    GfxAabbTree &tree1 = sw->trees[1];
    tree1.mins[0] = 512, tree1.mins[1] = -256, tree1.mins[2] = 0;
    tree1.maxs[0] = 1024, tree1.maxs[1] = 256, tree1.maxs[2] = 256;
    tree1.startSurfIndex = 6, tree1.surfaceCount = 3;
    tree1.startSurfIndexNoDecal = 6, tree1.surfaceCountNoDecal = 3;

    // The emissive surface is reachable only through cell 0's cull group.
    GfxCullGroup &group = sw->cullGroups[0];
    std::memcpy(group.mins, boxes[9].mins, sizeof(group.mins));
    std::memcpy(group.maxs, boxes[9].maxs, sizeof(group.maxs));
    group.startSurfIndex = 9;
    group.surfaceCount = 1;
    sw->cell0CullGroups[0] = 0;

    for (int c = 0; c < kCellCount; ++c)
    {
        GfxCell &cell = sw->cells[c];
        cell.mins[0] = c == 0 ? 0.0f : 512.0f;
        cell.maxs[0] = c == 0 ? 512.0f : 1024.0f;
        cell.mins[1] = -256, cell.maxs[1] = 256;
        cell.mins[2] = 0, cell.maxs[2] = 256;
        cell.aabbTreeCount = 1;
        cell.aabbTree = &sw->trees[c];
        cell.portalCount = 1;
        cell.portals = &sw->portals[c];
    }
    sw->cells[0].cullGroupCount = 1;
    sw->cells[0].cullGroups = sw->cell0CullGroups;
    SetPortal(sw->portals[0], sw->portalVerts[0], 512.0f, 1.0f, &sw->cells[1]);
    SetPortal(sw->portals[1], sw->portalVerts[1], 512.0f, -1.0f, &sw->cells[0]);

    // One splitting plane x = 512. R_CellForPoint walks uint16 nodes: an inner
    // node is {cellCount + 1 + planeIndex, rightChildOffset}; its front child
    // follows it and its back child sits rightChildOffset entries further on. A
    // leaf is {cellIndex + 1}.
    sw->planes[0].normal[0] = 1.0f;
    sw->planes[0].dist = 512.0f;
    sw->nodes[0] = kCellCount + 1 + 0;
    sw->nodes[1] = 3;
    sw->nodes[2] = 1 + 1; // front: cell 1
    sw->nodes[3] = 0 + 1; // back: cell 0
    w.planeCount = 1;
    w.nodeCount = 3;
    w.dpvsPlanes.cellCount = kCellCount;
    w.dpvsPlanes.planes = sw->planes;
    w.dpvsPlanes.nodes = sw->nodes;
    sw->sceneEntCellBits = std::make_unique<uint32_t[]>(2 * kCellCount * 128);
    w.dpvsPlanes.sceneEntCellBits = sw->sceneEntCellBits.get();
    w.cellBitsCount = 4;
    w.cells = sw->cells;

    w.surfaceCount = kSurfaceCount;
    w.indexCount = 3;
    w.indices = sw->indices;
    w.models = sw->models;
    w.modelCount = 1;
    sw->models[0].surfaceCount = kSurfaceCount;
    w.mins[0] = 0, w.mins[1] = -256, w.mins[2] = 0;
    w.maxs[0] = 1024, w.maxs[1] = 256, w.maxs[2] = 256;
    w.sunLight = &sw->sunLight;
    w.primaryLightCount = 1;

    GfxWorldDpvsStatic &dpvs = w.dpvs;
    dpvs.staticSurfaceCount = kSurfaceCount;
    dpvs.staticSurfaceCountNoDecal = kSurfaceCount - 1;
    dpvs.litSurfsBegin = kLitBegin;
    dpvs.litSurfsEnd = kLitEnd;
    dpvs.decalSurfsBegin = kDecalBegin;
    dpvs.decalSurfsEnd = kDecalEnd;
    dpvs.emissiveSurfsBegin = kEmissiveBegin;
    dpvs.emissiveSurfsEnd = kEmissiveEnd;
    dpvs.surfaceVisDataCount = kSurfaceCount;
    for (int v = 0; v < 3; ++v)
    {
        dpvs.surfaceVisData[v] = sw->surfaceVis[v];
        dpvs.smodelVisData[v] = sw->smodelVis[v];
    }
    dpvs.sortedSurfIndex = sw->sortedSurfIndex;
    dpvs.surfaces = sw->surfaces;
    dpvs.cullGroups = sw->cullGroups;
    dpvs.surfaceMaterials = sw->surfaceMaterials;
    dpvs.surfaceCastsSunShadow = sw->surfaceCastsSunShadow;
    w.cullGroupCount = 1;
    for (int d = 0; d < 2; ++d)
    {
        w.dpvsDyn.dynEntCellBits[d] = sw->dynEntCellBits[d];
        for (int v = 0; v < 3; ++v)
            w.dpvsDyn.dynEntVisData[d][v] = sw->dynEntVis[d][v];
    }
    w.sceneDynModel = sw->sceneDynModel;
    w.sceneDynBrush = sw->sceneDynBrush;
    return sw;
}

// ---------------------------------------------------------------------------
// Frame driver
// ---------------------------------------------------------------------------

std::unique_ptr<GfxBackEndData> g_backEnd;

// The view a refdef describes, through the production R_SetViewParmsForScene.
// zNear stays 0, so the near plane comes from r_znear (R_GetDefaultNearClip).
void SetViewParms(const float *origin, const float (*axis)[3], float tanHalfFov, GfxViewParms *viewParms)
{
    auto refdef = std::make_unique<refdef_s>(); // 16 KiB, zeroed
    for (int i = 0; i < 3; ++i)
    {
        refdef->vieworg[i] = origin[i];
        for (int j = 0; j < 3; ++j)
            refdef->viewaxis[i][j] = axis[i][j];
    }
    refdef->tanHalfFovX = tanHalfFov;
    refdef->tanHalfFovY = tanHalfFov;
    R_SetViewParmsForScene(refdef.get(), viewParms);
}

struct DrawSurfBatch
{
    uint32_t materialSortedIndex;
    uint32_t primarySortKey;
    std::vector<uint16_t> surfaces;
};

// Independent reader for the primitive stream the BSP draw path consumes:
// repeated {count, ceil(count / 2) words of packed uint16 surface indices},
// ended by a zero count (r_draw_bsp.cpp's R_ReadBspDrawSurfs contract).
bool ReadBatch(const GfxDrawSurf &drawSurf, DrawSurfBatch &out, std::string &error)
{
    out.materialSortedIndex = drawSurf.fields.materialSortedIndex;
    out.primarySortKey = drawSurf.fields.primarySortKey;
    out.surfaces.clear();
    uint32_t pos = drawSurf.fields.objectId;
    for (int guard = 0; guard < 64; ++guard)
    {
        if (pos >= 65536)
        {
            error = "primitive stream runs past the buffer";
            return false;
        }
        const uint32_t count = g_backEnd->primDrawSurfsBuf[pos++];
        if (count == 0)
            return true;
        const auto *list = reinterpret_cast<const uint16_t *>(&g_backEnd->primDrawSurfsBuf[pos]);
        out.surfaces.insert(out.surfaces.end(), list, list + count);
        pos += (count + 1) >> 1;
    }
    error = "primitive stream has no terminator";
    return false;
}

struct FrameResult
{
    int cameraCell = -2;
    std::set<int> visibleSurfaces;
    std::set<int> visibleCells;
    std::vector<DrawSurfBatch> lit;
    std::vector<DrawSurfBatch> decal;
    std::vector<DrawSurfBatch> emissive;
    std::string error;
};

std::vector<DrawSurfBatch> Emit(DrawSurfType stage, std::string &error)
{
    const int first = g_backEnd->drawSurfCount;
    R_MergeAndEmitDrawSurfLists(stage, 1);
    std::vector<DrawSurfBatch> batches;
    for (int i = first; i < g_backEnd->drawSurfCount; ++i)
    {
        DrawSurfBatch batch;
        if (!ReadBatch(g_backEnd->drawSurfs[i], batch, error))
            break;
        batches.push_back(batch);
    }
    return batches;
}

constexpr int kUseCameraCell = -100;

FrameResult RunFrame(SyntheticWorld &sw, const float *origin, const float (*axis)[3], float tanHalfFov,
                     int forcedCell = kUseCameraCell)
{
    FrameResult result;
    std::memset(g_backEnd.get(), 0, sizeof(GfxBackEndData));
    frontEndDataOut = g_backEnd.get();
    for (volatile long &count : scene.drawSurfCount)
        count = 0;

    R_ClearDpvsScene();

    GfxViewParms viewParms;
    SetViewParms(origin, axis, tanHalfFov, &viewParms);
    R_SetupWorldSurfacesDpvs(&viewParms);
    result.cameraCell = R_CellForPoint(&sw.world, viewParms.origin);
    R_AddWorldSurfacesDpvs(&viewParms, forcedCell == kUseCameraCell ? result.cameraCell : forcedCell);
    R_WaitWorkerCmdsOfType(WRKCMD_DPVS_CELL_STATIC);

    for (int i = 0; i < kSurfaceCount; ++i)
        if (sw.world.dpvs.surfaceVisData[0][i])
            result.visibleSurfaces.insert(i);
    for (int c = 0; c < kCellCount; ++c)
        if (dpvsGlob.cellVisibleBits[c >> 5] & (1u << (c & 31)))
            result.visibleCells.insert(c);

    R_AddAllBspDrawSurfacesCamera();
    R_AddAllBspDrawSurfacesCameraNonlit(sw.world.dpvs.emissiveSurfsBegin, sw.world.dpvs.emissiveSurfsEnd,
                                        DRAW_SURF_BSP_CAMERA_EMISSIVE);
    result.lit = Emit(DRAW_SURF_BSP_CAMERA_LIT, result.error);
    result.decal = Emit(DRAW_SURF_BSP_CAMERA_DECAL, result.error);
    result.emissive = Emit(DRAW_SURF_BSP_CAMERA_EMISSIVE, result.error);
    return result;
}

std::string Describe(const std::set<int> &values)
{
    std::string text = "{";
    for (int v : values)
        text += (text.size() > 1 ? "," : "") + std::to_string(v);
    return text + "}";
}

std::string Describe(const std::vector<DrawSurfBatch> &batches)
{
    std::string text;
    for (const DrawSurfBatch &batch : batches)
    {
        text += "[mtl " + std::to_string(batch.materialSortedIndex) + ":";
        for (uint16_t s : batch.surfaces)
            text += " " + std::to_string(s);
        text += "]";
    }
    return text.empty() ? "none" : text;
}

bool SameBatches(const std::vector<DrawSurfBatch> &actual,
                 const std::vector<std::pair<uint32_t, std::vector<uint16_t>>> &expected)
{
    if (actual.size() != expected.size())
        return false;
    for (size_t i = 0; i < actual.size(); ++i)
        if (actual[i].materialSortedIndex != expected[i].first || actual[i].surfaces != expected[i].second)
            return false;
    return true;
}

void SetBool(const dvar_s *dvar, bool value)
{
    Dvar_SetBool(const_cast<dvar_s *>(dvar), value);
}

void SetFloat(const dvar_s *dvar, float value)
{
    Dvar_SetFloat(const_cast<dvar_s *>(dvar), value);
}
} // namespace

int main()
{
    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    SL_Init();
    Cmd_InitCore();
    Dvar_Init();
    // common.cpp registers this before the renderer; with it off the worker
    // thread dvars register disabled and front-end work runs on this thread.
    sys_smp_allowed = Dvar_RegisterBool("sys_smp_allowed", false, DVAR_INIT, "Allow multi-threading");
    R_RegisterDvars();

    // Front-end work runs inline on this thread: no worker threads exist here.
    SetBool(r_smp_worker, false);
    // The pre-tessellated path writes into GPU index buffers; keep the CPU path.
    SetBool(r_pretess, false);
    SetFloat(r_zfar, 0.0f);

    gfxCfg.maxClientViews = 1;
    gfxCfg.entCount = 1024;
    gfxCfg.entnumNone = 1023;
    gfxCfg.entnumOrdinaryEnd = 1022;
    rg.drawWorld = r_drawWorld->current.enabled;
    rg.drawSModels = r_drawSModels->current.enabled;
    rg.drawBModels = r_drawBModels->current.enabled;
    rg.drawXModels = r_drawXModels->current.enabled;
    rg.lodParms.valid = 1;
    rg.correctedLodParms.valid = 1;
    Check(rg.drawWorld, "r_drawWorld defaults on");

    g_backEnd = std::make_unique<GfxBackEndData>();
    frontEndDataOut = g_backEnd.get();

    std::unique_ptr<SyntheticWorld> sw = BuildWorld();
    rgp.world = &sw->world;
    R_InitSceneBuffers();
    R_InitSceneData(0);
    R_InitScene();
    Check(scene.drawSurfs[DRAW_SURF_BSP_CAMERA_DECAL] == scene.bspDrawSurfsDecal &&
              scene.drawSurfs[DRAW_SURF_BSP_CAMERA_EMISSIVE] == scene.bspDrawSurfsEmissive &&
              scene.maxDrawSurfCount[DRAW_SURF_BSP_CAMERA_LIT] == 0x2000 &&
              scene.maxDrawSurfCount[DRAW_SURF_BSP_CAMERA_DECAL] == 512,
          "R_InitScene points the BSP stages at the scene's own lists with their capacities");

    const float forward[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float eye0[3] = {64, 0, 128};

    {
        GfxViewParms viewParms;
        SetViewParms(eye0, forward, 1.0f, &viewParms);
        Check(viewParms.zNear == r_znear->current.value && viewParms.origin[3] == 1.0f &&
                  viewParms.depthHackNearClip == -r_znear_depthhack->current.value,
              "R_SetViewParmsForScene takes the near planes from r_znear and r_znear_depthhack");
    }

    // ---- view 1: inside cell 0 looking through the portal --------------------
    {
        Check(R_CellForPoint(&sw->world, eye0) == 0, "R_CellForPoint puts the eye at x=64 in cell 0");
        const float inCell1[3] = {700, 0, 128};
        Check(R_CellForPoint(&sw->world, inCell1) == 1, "R_CellForPoint puts x=700 in cell 1");

        FrameResult frame = RunFrame(*sw, eye0, forward, 1.0f);
        Check(frame.error.empty(), "primitive streams decode: " + (frame.error.empty() ? "ok" : frame.error));
        Check(frame.cameraCell == 0, "camera cell is 0");
        Check(frame.visibleCells == std::set<int>({0, 1}),
              "portal walk marks both cells visible: " + Describe(frame.visibleCells));
        const std::set<int> expectedVisible = {0, 1, 3, 4, 5, 6, 8, 9};
        Check(frame.visibleSurfaces == expectedVisible,
              "visible surfaces " + Describe(frame.visibleSurfaces) + " == " + Describe(expectedVisible));
        Check(!frame.visibleSurfaces.count(2), "surface behind the eye is culled by the near plane");
        Check(!frame.visibleSurfaces.count(7), "surface outside the portal window is culled by portal planes");
        Check(SameBatches(frame.lit, {{5, {0, 1, 3, 4}}, {9, {5, 6}}}),
              "lit stage batches A{0,1,3,4} then B{5,6}: " + Describe(frame.lit));
        Check(SameBatches(frame.decal, {{3, {8}}}), "decal stage holds C{8}: " + Describe(frame.decal));
        Check(SameBatches(frame.emissive, {{12, {9}}}), "emissive range holds D{9}: " + Describe(frame.emissive));
    }

    // ---- view 1 again with a far plane ---------------------------------------
    {
        SetFloat(r_zfar, 430.0f);
        FrameResult frame = RunFrame(*sw, eye0, forward, 1.0f);
        SetFloat(r_zfar, 0.0f);
        // Distance along +x from the eye: surface 3 spans 416..436, so a far
        // plane at 430 keeps it (DPVS keeps boxes that straddle a plane). The
        // portal is 448 away, wholly past that plane, so cell 1 is never entered.
        const std::set<int> expected = {0, 1, 3, 5, 8, 9};
        Check(frame.visibleSurfaces == expected,
              "r_zfar 430 keeps straddling surface 3 and drops cell 1: " + Describe(frame.visibleSurfaces));
        Check(frame.visibleCells == std::set<int>({0}), "r_zfar 430 stops the walk at the portal: " +
                                                            Describe(frame.visibleCells));
        SetFloat(r_zfar, 400.0f);
        frame = RunFrame(*sw, eye0, forward, 1.0f);
        SetFloat(r_zfar, 0.0f);
        Check(frame.visibleSurfaces == std::set<int>({0, 1, 5, 8, 9}),
              "r_zfar 400 also culls surface 3: " + Describe(frame.visibleSurfaces));
        Check(SameBatches(frame.lit, {{5, {0, 1}}, {9, {5}}}),
              "far-plane lit batches A{0,1} then B{5}: " + Describe(frame.lit));
    }

    // ---- view 2: inside cell 1 looking back at the portal --------------------
    {
        const float back[3][3] = {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
        const float eye1[3] = {960, 0, 128};
        FrameResult frame = RunFrame(*sw, eye1, back, 1.0f);
        Check(frame.cameraCell == 1, "camera cell is 1 for the reverse view");
        Check(frame.visibleCells == std::set<int>({0, 1}),
              "reverse walk reaches cell 0: " + Describe(frame.visibleCells));
        // Looking down -x from x=960 with a 90 degree frustum, a box at depth d
        // is inside when it reaches |y| < d and |z - 128| < d. Cell 1: surfaces 4
        // (d 40..60) and 6 (d 140..160) pass; surface 7 needs |y| >= 180 > 160 and
        // is culled. Through the window (d = 448) the bound narrows to 64 * d / 448;
        // every cell 0 box reaches inside it, including surface 2 behind the first
        // view's eye.
        const std::set<int> expected = {0, 1, 2, 3, 4, 5, 6, 8, 9};
        Check(frame.visibleSurfaces == expected,
              "reverse view sees " + Describe(frame.visibleSurfaces) + " == " + Describe(expected));
        Check(SameBatches(frame.lit, {{5, {0, 1, 2, 3, 4}}, {9, {5, 6}}}),
              "reverse lit batches A{0,1,2,3,4} then B{5,6}: " + Describe(frame.lit));
    }

    // ---- no camera cell: every cell against the frustum alone ----------------
    {
        // With one splitting plane every point lands in a cell; a real world has
        // solid leaves that return -1. Passing -1 takes the same branch.
        const float outside[3] = {-200, 0, 128};
        Check(R_CellForPoint(&sw->world, outside) == 0, "a single-plane tree has no solid leaf");
        FrameResult frame = RunFrame(*sw, eye0, forward, 1.0f, -1);
        // Without the portal, surface 7 (y 180..240 at depth 736..756) is inside
        // the camera frustum; only surface 2 behind the eye is culled.
        const std::set<int> expected = {0, 1, 3, 4, 5, 6, 7, 8, 9};
        Check(frame.visibleSurfaces == expected,
              "no-cell path tests every cell by frustum: " + Describe(frame.visibleSurfaces));
        Check(SameBatches(frame.lit, {{5, {0, 1, 3, 4}}, {9, {5, 6, 7}}}),
              "no-cell lit batches A{0,1,3,4} then B{5,6,7}: " + Describe(frame.lit));
    }

    // ---- r_skipPvs: nothing is visited ----------------------------------------
    {
        SetBool(r_skipPvs, true);
        FrameResult frame = RunFrame(*sw, eye0, forward, 1.0f);
        SetBool(r_skipPvs, false);
        Check(frame.visibleSurfaces.empty() && frame.lit.empty() && frame.decal.empty(),
              "r_skipPvs leaves every surface invisible and emits no batches");
    }

    // ---- r_drawDecals off: the decal stage is empty --------------------------
    {
        SetBool(r_drawDecals, false);
        FrameResult frame = RunFrame(*sw, eye0, forward, 1.0f);
        SetBool(r_drawDecals, true);
        Check(!frame.visibleSurfaces.count(8) && frame.decal.empty(),
              "r_drawDecals 0 drops the decal surface through the AABB tree's no-decal range");
        Check(SameBatches(frame.lit, {{5, {0, 1, 3, 4}}, {9, {5, 6}}}), "lit batches unchanged without decals");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("PASS: world front end\n");
    return g_failures == 0 ? 0 : 1;
}
