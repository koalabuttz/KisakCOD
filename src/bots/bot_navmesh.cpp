// The bots' navigation mesh (bot_navmesh.h, docs/bots.md): a tiled Recast
// build from a triangle soup, a cache blob, and Detour queries.

#include "bot_navmesh.h"

#include <recastnavigation/Recast/Recast.h>
#include <recastnavigation/Detour/DetourAlloc.h>
#include <recastnavigation/Detour/DetourNavMesh.h>
#include <recastnavigation/Detour/DetourNavMeshBuilder.h>
#include <recastnavigation/Detour/DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace kisak::bots
{

namespace
{

// "KBNV": KisakCOD bot navigation. Bump the version whenever the build or
// the layout changes, so every cache rebuilds.
constexpr uint32_t kBlobMagic = 0x564e424bu;
// Version 2: drop-down links (DropLinks below).
constexpr uint32_t kBlobVersion = 2;
constexpr int kMaxVertsPerPoly = 6;
constexpr unsigned short kPolyWalk = 0x01;
constexpr unsigned char kAreaGround = 1;
constexpr unsigned char kAreaDrop = 2;
// A drop costs this much more than walking as far, so a bot takes the stairs
// when they are not much longer.
constexpr float kDropCost = 1.5f;

struct BlobHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t mapChecksum;
    uint32_t tileCount;
    float origin[3];
    float tileWidth;
    float tileHeight;
    int32_t maxTiles;
    int32_t maxPolys;
};

struct TileHeader
{
    uint32_t size;
};

// Game (x, y, z) to Recast (x, z, -y): a rotation about x, so windings and
// the side a triangle faces are kept. And back.
inline void ToRecast(const float *in, float *out)
{
    out[0] = in[0];
    out[1] = in[2];
    out[2] = -in[1];
}

inline void FromRecast(const float *in, float *out)
{
    out[0] = in[0];
    out[1] = -in[2];
    out[2] = in[1];
}

void SetError(char *error, size_t size, const char *text)
{
    if (error && size)
        std::snprintf(error, size, "%s", text);
}

int NextPowerOfTwoBits(unsigned int value)
{
    int bits = 0;
    while ((1u << bits) < value)
        ++bits;
    return bits;
}

// Everything one tile's build owns, freed together.
struct TileWork
{
    rcHeightfield *solid = nullptr;
    rcCompactHeightfield *compact = nullptr;
    rcContourSet *contours = nullptr;

    ~TileWork()
    {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(compact);
        rcFreeContourSet(contours);
    }
};

struct PolyMeshFree
{
    void operator()(rcPolyMesh *mesh) const { rcFreePolyMesh(mesh); }
};

struct DetailFree
{
    void operator()(rcPolyMeshDetail *detail) const { rcFreePolyMeshDetail(detail); }
};

// One tile's polygons, kept until its Detour data is made for the last time
// (with its drop-downs).
struct TileMesh
{
    int tx = 0;
    int ty = 0;
    std::unique_ptr<rcPolyMesh, PolyMeshFree> mesh;
    std::unique_ptr<rcPolyMeshDetail, DetailFree> detail;
};

// A drop-down: from the edge of a floor to the floor below, in Recast
// coordinates. Walked one way only.
struct DropLink
{
    float start[3];
    float end[3];
};

// One tile: rasterise the triangles that touch it (with a border so the
// edges match the neighbours), filter, erode by the player's radius, build
// regions, contours, polygons and the detail mesh. False when the tile has
// no floor.
bool BuildTile(rcContext &ctx,
               const rcConfig &base,
               const float *verts,
               const std::vector<int> &tris,
               const std::vector<unsigned char> &areas,
               int tx,
               int ty,
               const NavBuildParams &params,
               TileMesh &out)
{
    rcConfig cfg = base;
    const float tileWorld = params.tileCells * cfg.cs;
    cfg.bmin[0] = base.bmin[0] + tx * tileWorld;
    cfg.bmin[2] = base.bmin[2] + ty * tileWorld;
    cfg.bmax[0] = cfg.bmin[0] + tileWorld;
    cfg.bmax[2] = cfg.bmin[2] + tileWorld;
    cfg.bmin[0] -= cfg.borderSize * cfg.cs;
    cfg.bmin[2] -= cfg.borderSize * cfg.cs;
    cfg.bmax[0] += cfg.borderSize * cfg.cs;
    cfg.bmax[2] += cfg.borderSize * cfg.cs;

    TileWork work;
    work.solid = rcAllocHeightfield();
    if (!work.solid || !rcCreateHeightfield(&ctx, *work.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        return false;

    bool any = false;
    const int triCount = static_cast<int>(tris.size() / 3);
    for (int i = 0; i < triCount; ++i)
    {
        const float *a = &verts[tris[i * 3 + 0] * 3];
        const float *b = &verts[tris[i * 3 + 1] * 3];
        const float *c = &verts[tris[i * 3 + 2] * 3];
        const float minX = std::min(a[0], std::min(b[0], c[0]));
        const float maxX = std::max(a[0], std::max(b[0], c[0]));
        const float minZ = std::min(a[2], std::min(b[2], c[2]));
        const float maxZ = std::max(a[2], std::max(b[2], c[2]));
        if (maxX < cfg.bmin[0] || minX > cfg.bmax[0] || maxZ < cfg.bmin[2] || minZ > cfg.bmax[2])
            continue;
        if (!rcRasterizeTriangle(&ctx, a, b, c, areas[i], *work.solid, cfg.walkableClimb))
            return false;
        any = true;
    }
    if (!any)
        return false;

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *work.solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *work.solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *work.solid);

    work.compact = rcAllocCompactHeightfield();
    if (!work.compact
        || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *work.solid, *work.compact))
        return false;
    rcFreeHeightField(work.solid);
    work.solid = nullptr;

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *work.compact))
        return false;
    if (!rcBuildDistanceField(&ctx, *work.compact))
        return false;
    if (!rcBuildRegions(&ctx, *work.compact, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
        return false;

    work.contours = rcAllocContourSet();
    if (!work.contours
        || !rcBuildContours(&ctx, *work.compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *work.contours))
        return false;
    if (work.contours->nconts == 0)
        return false;

    out.mesh.reset(rcAllocPolyMesh());
    if (!out.mesh || !rcBuildPolyMesh(&ctx, *work.contours, cfg.maxVertsPerPoly, *out.mesh))
        return false;
    out.detail.reset(rcAllocPolyMeshDetail());
    if (!out.detail
        || !rcBuildPolyMeshDetail(&ctx, *out.mesh, *work.compact, cfg.detailSampleDist, cfg.detailSampleMaxError,
                                  *out.detail))
        return false;
    // Room for each drop-down's two points, too.
    if (out.mesh->npolys == 0 || out.mesh->nverts >= 0xffff - 2 * 256)
        return false;

    for (int i = 0; i < out.mesh->npolys; ++i)
    {
        if (out.mesh->areas[i] == RC_WALKABLE_AREA)
            out.mesh->areas[i] = kAreaGround;
        out.mesh->flags[i] = kPolyWalk;
    }
    out.tx = tx;
    out.ty = ty;
    return true;
}

// A tile's Detour data (dtAlloc'd) with the drop-downs that start in it, or
// nullptr.
unsigned char *TileData(const TileMesh &tile,
                        const rcConfig &cfg,
                        const NavBuildParams &params,
                        const std::vector<DropLink> &links,
                        int *dataSize)
{
    const rcPolyMesh &mesh = *tile.mesh;
    const rcPolyMeshDetail &detail = *tile.detail;
    dtNavMeshCreateParams create;
    std::memset(&create, 0, sizeof(create));
    create.verts = mesh.verts;
    create.vertCount = mesh.nverts;
    create.polys = mesh.polys;
    create.polyAreas = mesh.areas;
    create.polyFlags = mesh.flags;
    create.polyCount = mesh.npolys;
    create.nvp = mesh.nvp;
    create.detailMeshes = detail.meshes;
    create.detailVerts = detail.verts;
    create.detailVertsCount = detail.nverts;
    create.detailTris = detail.tris;
    create.detailTriCount = detail.ntris;

    const int linkCount = static_cast<int>(links.size());
    std::vector<float> linkVerts(static_cast<size_t>(linkCount) * 6);
    std::vector<float> linkRad(linkCount, 12.0f);
    std::vector<unsigned short> linkFlags(linkCount, kPolyWalk);
    std::vector<unsigned char> linkAreas(linkCount, kAreaDrop);
    std::vector<unsigned char> linkDir(linkCount, 0);
    std::vector<unsigned int> linkIds(linkCount);
    for (int i = 0; i < linkCount; ++i)
    {
        rcVcopy(&linkVerts[i * 6], links[i].start);
        rcVcopy(&linkVerts[i * 6 + 3], links[i].end);
        linkIds[i] = static_cast<unsigned int>(i);
    }
    if (linkCount)
    {
        create.offMeshConVerts = linkVerts.data();
        create.offMeshConRad = linkRad.data();
        create.offMeshConFlags = linkFlags.data();
        create.offMeshConAreas = linkAreas.data();
        create.offMeshConDir = linkDir.data();
        create.offMeshConUserID = linkIds.data();
        create.offMeshConCount = linkCount;
    }

    create.walkableHeight = params.agentHeight;
    create.walkableRadius = params.agentRadius;
    create.walkableClimb = params.agentClimb;
    create.tileX = tile.tx;
    create.tileY = tile.ty;
    create.tileLayer = 0;
    rcVcopy(create.bmin, mesh.bmin);
    rcVcopy(create.bmax, mesh.bmax);
    create.cs = cfg.cs;
    create.ch = cfg.ch;
    create.buildBvTree = true;

    unsigned char *data = nullptr;
    if (!dtCreateNavMeshData(&create, &data, dataSize))
        return nullptr;
    return data;
}

// The map's triangles in a grid of columns (Recast x, z), for asking whether
// a short segment passes through anything solid.
class TriangleGrid
{
public:
    TriangleGrid(const float *verts, const std::vector<int> &tris, const float *bmin, const float *bmax)
        : m_verts(verts), m_tris(tris)
    {
        m_origin[0] = bmin[0];
        m_origin[1] = bmin[2];
        m_width = std::max(1, static_cast<int>(std::ceil((bmax[0] - bmin[0]) / kCell)) + 1);
        m_height = std::max(1, static_cast<int>(std::ceil((bmax[2] - bmin[2]) / kCell)) + 1);
        const int triCount = static_cast<int>(tris.size() / 3);
        m_stamp.assign(triCount, 0);
        std::vector<int> counts(static_cast<size_t>(m_width) * m_height + 1, 0);
        // Two passes: count each cell's triangles, then fill.
        for (int pass = 0; pass < 2; ++pass)
        {
            if (pass == 1)
            {
                m_start.assign(counts.size(), 0);
                for (size_t i = 1; i < counts.size(); ++i)
                    m_start[i] = m_start[i - 1] + counts[i - 1];
                m_items.assign(m_start.back(), 0);
                std::fill(counts.begin(), counts.end(), 0);
            }
            for (int t = 0; t < triCount; ++t)
            {
                int x0, z0, x1, z1;
                TriangleCells(t, &x0, &z0, &x1, &z1);
                for (int z = z0; z <= z1; ++z)
                {
                    for (int x = x0; x <= x1; ++x)
                    {
                        const int cell = z * m_width + x;
                        if (pass == 1)
                            m_items[m_start[cell] + counts[cell]] = t;
                        ++counts[cell];
                    }
                }
            }
        }
    }

    // Whether the segment from `a` to `b` passes through a triangle.
    bool Blocked(const float *a, const float *b)
    {
        ++m_query;
        const int x0 = Cell(std::min(a[0], b[0]), 0);
        const int x1 = Cell(std::max(a[0], b[0]), 0);
        const int z0 = Cell(std::min(a[2], b[2]), 1);
        const int z1 = Cell(std::max(a[2], b[2]), 1);
        for (int z = z0; z <= z1; ++z)
        {
            for (int x = x0; x <= x1; ++x)
            {
                const int cell = z * m_width + x;
                for (int i = m_start[cell]; i < m_start[cell + 1]; ++i)
                {
                    const int t = m_items[i];
                    if (m_stamp[t] == m_query)
                        continue;
                    m_stamp[t] = m_query;
                    if (SegmentHits(a, b, &m_verts[m_tris[t * 3 + 0] * 3], &m_verts[m_tris[t * 3 + 1] * 3],
                                    &m_verts[m_tris[t * 3 + 2] * 3]))
                        return true;
                }
            }
        }
        return false;
    }

private:
    static constexpr float kCell = 64.0f;

    int Cell(float value, int axis) const
    {
        const int limit = axis == 0 ? m_width : m_height;
        const int cell = static_cast<int>(std::floor((value - m_origin[axis]) / kCell));
        return std::clamp(cell, 0, limit - 1);
    }

    void TriangleCells(int t, int *x0, int *z0, int *x1, int *z1) const
    {
        const float *a = &m_verts[m_tris[t * 3 + 0] * 3];
        const float *b = &m_verts[m_tris[t * 3 + 1] * 3];
        const float *c = &m_verts[m_tris[t * 3 + 2] * 3];
        *x0 = Cell(std::min(a[0], std::min(b[0], c[0])), 0);
        *x1 = Cell(std::max(a[0], std::max(b[0], c[0])), 0);
        *z0 = Cell(std::min(a[2], std::min(b[2], c[2])), 1);
        *z1 = Cell(std::max(a[2], std::max(b[2], c[2])), 1);
    }

    // Moller-Trumbore, either side of the triangle.
    static bool SegmentHits(const float *p, const float *q, const float *a, const float *b, const float *c)
    {
        float d[3], e1[3], e2[3], h[3], s[3], k[3];
        rcVsub(d, q, p);
        rcVsub(e1, b, a);
        rcVsub(e2, c, a);
        rcVcross(h, d, e2);
        const float det = rcVdot(e1, h);
        if (std::fabs(det) < 1.0e-6f)
            return false;
        const float inv = 1.0f / det;
        rcVsub(s, p, a);
        const float u = inv * rcVdot(s, h);
        if (u < 0.0f || u > 1.0f)
            return false;
        rcVcross(k, s, e1);
        const float v = inv * rcVdot(d, k);
        if (v < 0.0f || u + v > 1.0f)
            return false;
        const float t = inv * rcVdot(e2, k);
        return t >= 0.0f && t <= 1.0f;
    }

    const float *m_verts;
    const std::vector<int> &m_tris;
    float m_origin[2] = {};
    int m_width = 1;
    int m_height = 1;
    std::vector<int> m_start;
    std::vector<int> m_items;
    std::vector<uint32_t> m_stamp;
    uint32_t m_query = 0;
};

// Drop-downs. Recast links floors a player walks between; a floor reached
// only by stepping off a ledge (a balcony over a street, a landing over a
// room) is left an island the bots never go to. For every edge of the mesh
// with nothing beyond it, look a little past it for a floor below, between a
// step (agentClimb) and the height that starts fall damage, with nothing
// solid between: no railing, wall or window frame across the way out, no
// floor between the two.
constexpr float kDropSpacing = 64.0f;
constexpr float kDropMin = 21.0f;
// bg_fallDamageMinHeight is 128; the mesh's heights may be a few units off.
constexpr float kDropMax = 116.0f;
constexpr int kMaxDropsPerTile = 256;

bool DropClear(TriangleGrid &grid, const float *from, const float *out, const float *land, const float *along)
{
    // Three heights a standing player fills, at its middle and both sides.
    static const float kHeights[] = {22.0f, 44.0f, 66.0f};
    static const float kSides[] = {-12.0f, 0.0f, 12.0f};
    for (float side : kSides)
    {
        const float dx = along[0] * side;
        const float dz = along[2] * side;
        for (float height : kHeights)
        {
            const float a[3] = {from[0] + dx, from[1] + height, from[2] + dz};
            const float b[3] = {out[0] + dx, from[1] + height, out[2] + dz};
            if (grid.Blocked(a, b))
                return false;
        }
        const float top[3] = {land[0] + dx, from[1] + kHeights[0], land[2] + dz};
        const float bottom[3] = {land[0] + dx, land[1] + 8.0f, land[2] + dz};
        if (grid.Blocked(top, bottom))
            return false;
    }
    return true;
}

// The drop-downs that start in each tile of `mesh`, by tile index in `tiles`.
int FindDropLinks(const dtNavMesh &mesh,
                  const dtNavMeshQuery &query,
                  const dtQueryFilter &filter,
                  TriangleGrid &grid,
                  const std::vector<TileMesh> &tiles,
                  int maxPolys,
                  std::vector<std::vector<DropLink>> &links)
{
    links.assign(tiles.size(), {});
    int total = 0;
    for (size_t index = 0; index < tiles.size(); ++index)
    {
        const dtMeshTile *tile = mesh.getTileAt(tiles[index].tx, tiles[index].ty, 0);
        if (!tile || !tile->header)
            continue;
        std::vector<DropLink> &found = links[index];
        // Each drop-down is a polygon of the tile too.
        const int limit = std::min(kMaxDropsPerTile, maxPolys - tiles[index].mesh->npolys);
        const dtPolyRef base = mesh.getPolyRefBase(tile);
        for (int ip = 0; ip < tile->header->polyCount && static_cast<int>(found.size()) < limit; ++ip)
        {
            const dtPoly &poly = tile->polys[ip];
            if (poly.getType() != DT_POLYTYPE_GROUND)
                continue;
            const int nv = poly.vertCount;
            float centre[3] = {0.0f, 0.0f, 0.0f};
            for (int j = 0; j < nv; ++j)
                rcVadd(centre, centre, &tile->verts[poly.verts[j] * 3]);
            for (float &axis : centre)
                axis /= static_cast<float>(nv);

            for (int j = 0; j < nv; ++j)
            {
                bool linked = false;
                for (unsigned int k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
                {
                    if (tile->links[k].edge == j)
                    {
                        linked = true;
                        break;
                    }
                }
                if (linked)
                    continue;
                const float *va = &tile->verts[poly.verts[j] * 3];
                const float *vb = &tile->verts[poly.verts[(j + 1) % nv] * 3];
                const float ex = vb[0] - va[0];
                const float ez = vb[2] - va[2];
                const float length = std::sqrt(ex * ex + ez * ez);
                if (length < 1.0f)
                    continue;
                const float along[3] = {ex / length, 0.0f, ez / length};
                float normal[3] = {ez / length, 0.0f, -ex / length};
                const float mx = (va[0] + vb[0]) * 0.5f - centre[0];
                const float mz = (va[2] + vb[2]) * 0.5f - centre[2];
                if (normal[0] * mx + normal[2] * mz < 0.0f)
                {
                    normal[0] = -normal[0];
                    normal[2] = -normal[2];
                }
                const int samples = std::max(1, static_cast<int>(length / kDropSpacing));
                for (int si = 0; si < samples && static_cast<int>(found.size()) < limit; ++si)
                {
                    const float t = (si + 0.5f) / samples;
                    // A hair inside the polygon, and so inside the tile.
                    float point[3] = {va[0] + ex * t - normal[0], 0.0f, va[2] + ez * t - normal[2]};
                    point[1] = (va[1] + vb[1]) * 0.5f;
                    float from[3];
                    if (dtStatusFailed(query.closestPointOnPoly(base | static_cast<dtPolyRef>(ip), point, from,
                                                                nullptr)))
                        continue;
                    if (from[0] < tile->header->bmin[0] || from[0] >= tile->header->bmax[0]
                        || from[2] < tile->header->bmin[2] || from[2] >= tile->header->bmax[2])
                        continue;
                    for (float reach : {44.0f, 60.0f, 76.0f})
                    {
                        const float out[3] = {from[0] + normal[0] * reach, from[1], from[2] + normal[2] * reach};
                        const float centreBelow[3] = {out[0], from[1] - (kDropMin + kDropMax) * 0.5f, out[2]};
                        const float extents[3] = {8.0f, (kDropMax - kDropMin) * 0.5f, 8.0f};
                        dtPolyRef landRef = 0;
                        float land[3];
                        if (dtStatusFailed(query.findNearestPoly(centreBelow, extents, &filter, &landRef, land))
                            || !landRef)
                            continue;
                        const float drop = from[1] - land[1];
                        if (drop < kDropMin || drop > kDropMax
                            || std::hypot(land[0] - out[0], land[2] - out[2]) > 6.0f)
                            continue;
                        if (!DropClear(grid, from, out, land, along))
                            continue;
                        bool same = false;
                        for (const DropLink &other : found)
                        {
                            if (rcVdist(other.start, from) < 40.0f && rcVdist(other.end, land) < 40.0f)
                            {
                                same = true;
                                break;
                            }
                        }
                        if (!same)
                        {
                            DropLink link;
                            rcVcopy(link.start, from);
                            rcVcopy(link.end, land);
                            found.push_back(link);
                            ++total;
                        }
                        break;
                    }
                }
            }
        }
    }
    return total;
}

} // namespace

void NavGeometry::AddTriangle(const float *a, const float *b, const float *c)
{
    const int base = static_cast<int>(verts.size() / 3);
    for (const float *v : {a, b, c})
    {
        verts.push_back(v[0]);
        verts.push_back(v[1]);
        verts.push_back(v[2]);
    }
    tris.push_back(base);
    tris.push_back(base + 1);
    tris.push_back(base + 2);
}

void NavGeometry::AddPolygon(const float (*points)[3], int count)
{
    if (count < 3)
        return;
    const int base = static_cast<int>(verts.size() / 3);
    for (int i = 0; i < count; ++i)
    {
        verts.push_back(points[i][0]);
        verts.push_back(points[i][1]);
        verts.push_back(points[i][2]);
    }
    for (int i = 1; i + 1 < count; ++i)
    {
        tris.push_back(base);
        tris.push_back(base + i);
        tris.push_back(base + i + 1);
    }
}

bool BuildNavMesh(const NavGeometry &geometry,
                  const NavBuildParams &params,
                  uint32_t mapChecksum,
                  std::vector<uint8_t> &blob,
                  NavBuildStats *stats,
                  char *error,
                  size_t errorSize)
{
    blob.clear();
    NavBuildStats local;
    NavBuildStats &st = stats ? *stats : local;
    st = NavBuildStats();
    st.inputTriangles = geometry.TriangleCount();

    const int vertCount = static_cast<int>(geometry.verts.size() / 3);
    if (vertCount < 3 || geometry.tris.size() < 3 || geometry.tris.size() % 3)
    {
        SetError(error, errorSize, "no geometry");
        return false;
    }
    for (int index : geometry.tris)
    {
        if (index < 0 || index >= vertCount)
        {
            SetError(error, errorSize, "a triangle names a vertex that does not exist");
            return false;
        }
    }
    if (!(params.cellSize > 0.0f) || !(params.cellHeight > 0.0f) || params.tileCells < 16)
    {
        SetError(error, errorSize, "bad build parameters");
        return false;
    }

    std::vector<float> verts(geometry.verts.size());
    for (int i = 0; i < vertCount; ++i)
        ToRecast(&geometry.verts[i * 3], &verts[i * 3]);

    rcContext ctx(false);
    const int triCount = geometry.TriangleCount();
    std::vector<unsigned char> areas(triCount, 0);
    rcMarkWalkableTriangles(&ctx, params.maxSlopeDegrees, verts.data(), vertCount, geometry.tris.data(), triCount,
                            areas.data());

    rcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cs = params.cellSize;
    cfg.ch = params.cellHeight;
    cfg.walkableSlopeAngle = params.maxSlopeDegrees;
    cfg.walkableHeight = static_cast<int>(std::ceil(params.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(params.agentClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(params.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f * params.agentRadius / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = params.minRegionCells;
    cfg.mergeRegionArea = params.mergeRegionCells;
    cfg.maxVertsPerPoly = kMaxVertsPerPoly;
    cfg.tileSize = params.tileCells;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch * 1.0f;
    rcCalcBounds(verts.data(), vertCount, cfg.bmin, cfg.bmax);
    // Room above the highest floor for a standing player.
    cfg.bmax[1] += params.agentHeight;

    int gridW = 0;
    int gridH = 0;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &gridW, &gridH);
    const int tilesX = (gridW + params.tileCells - 1) / params.tileCells;
    const int tilesY = (gridH + params.tileCells - 1) / params.tileCells;
    st.tilesX = tilesX;
    st.tilesY = tilesY;
    if (tilesX <= 0 || tilesY <= 0 || tilesX * tilesY > (1 << 14))
    {
        SetError(error, errorSize, "the map is too large for the tile grid");
        return false;
    }
    if ((cfg.bmax[1] - cfg.bmin[1]) / cfg.ch >= static_cast<float>(RC_SPAN_MAX_HEIGHT))
    {
        SetError(error, errorSize, "the map is too tall for the cell height");
        return false;
    }

    const int tileBits = std::max(1, NextPowerOfTwoBits(static_cast<unsigned int>(tilesX * tilesY)));
    const int polyBits = 22 - tileBits;
    if (polyBits < 6)
    {
        SetError(error, errorSize, "too many tiles for 32-bit polygon references");
        return false;
    }

    struct DataFree
    {
        void operator()(unsigned char *data) const { dtFree(data); }
    };
    struct BuiltTile
    {
        std::unique_ptr<unsigned char, DataFree> data;
        int size = 0;
    };
    std::vector<TileMesh> tiles;
    std::vector<BuiltTile> built;
    const std::vector<DropLink> none;
    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            TileMesh tile;
            if (!BuildTile(ctx, cfg, verts.data(), geometry.tris, areas, tx, ty, params, tile))
                continue;
            const int polys = tile.mesh->npolys;
            if (polys > (1 << polyBits))
            {
                SetError(error, errorSize, "a tile has more polygons than its references can name");
                return false;
            }
            BuiltTile data;
            data.data.reset(TileData(tile, cfg, params, none, &data.size));
            if (!data.data)
                continue;
            tiles.push_back(std::move(tile));
            built.push_back(std::move(data));
            st.polygons += polys;
        }
    }
    st.tilesBuilt = static_cast<int>(built.size());
    if (built.empty())
    {
        SetError(error, errorSize, "no walkable floor");
        return false;
    }

    // The drop-downs, found on the mesh as built so far, then each tile with
    // any made again with them.
    {
        dtNavMeshParams nav;
        std::memset(&nav, 0, sizeof(nav));
        rcVcopy(nav.orig, cfg.bmin);
        nav.tileWidth = params.tileCells * cfg.cs;
        nav.tileHeight = params.tileCells * cfg.cs;
        nav.maxTiles = 1 << tileBits;
        nav.maxPolys = 1 << polyBits;
        struct MeshFree
        {
            void operator()(dtNavMesh *mesh) const { dtFreeNavMesh(mesh); }
        };
        struct QueryFree
        {
            void operator()(dtNavMeshQuery *query) const { dtFreeNavMeshQuery(query); }
        };
        std::unique_ptr<dtNavMesh, MeshFree> mesh(dtAllocNavMesh());
        std::unique_ptr<dtNavMeshQuery, QueryFree> query(dtAllocNavMeshQuery());
        bool ready = mesh && query && dtStatusSucceed(mesh->init(&nav));
        // The temporary mesh borrows the tiles' data (no DT_TILE_FREE_DATA).
        for (size_t i = 0; ready && i < built.size(); ++i)
            ready = dtStatusSucceed(mesh->addTile(built[i].data.get(), built[i].size, 0, 0, nullptr));
        ready = ready && dtStatusSucceed(query->init(mesh.get(), 2048));
        if (!ready)
        {
            SetError(error, errorSize, "the mesh did not load to look for drop-downs");
            return false;
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(kPolyWalk);
        filter.setExcludeFlags(0);
        TriangleGrid grid(verts.data(), geometry.tris, cfg.bmin, cfg.bmax);
        std::vector<std::vector<DropLink>> links;
        st.dropLinks = FindDropLinks(*mesh, *query, filter, grid, tiles, 1 << polyBits, links);
        query.reset();
        mesh.reset();
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            if (links[i].empty())
                continue;
            BuiltTile data;
            data.data.reset(TileData(tiles[i], cfg, params, links[i], &data.size));
            if (!data.data)
            {
                SetError(error, errorSize, "a tile with drop-downs did not build");
                return false;
            }
            built[i] = std::move(data);
        }
    }

    BlobHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = kBlobMagic;
    header.version = kBlobVersion;
    header.mapChecksum = mapChecksum;
    header.tileCount = static_cast<uint32_t>(built.size());
    header.origin[0] = cfg.bmin[0];
    header.origin[1] = cfg.bmin[1];
    header.origin[2] = cfg.bmin[2];
    header.tileWidth = params.tileCells * cfg.cs;
    header.tileHeight = params.tileCells * cfg.cs;
    header.maxTiles = 1 << tileBits;
    header.maxPolys = 1 << polyBits;

    size_t total = sizeof(header);
    for (const BuiltTile &tile : built)
        total += sizeof(TileHeader) + static_cast<size_t>(tile.size);
    blob.resize(total);
    uint8_t *out = blob.data();
    std::memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    for (BuiltTile &tile : built)
    {
        TileHeader th;
        th.size = static_cast<uint32_t>(tile.size);
        std::memcpy(out, &th, sizeof(th));
        out += sizeof(th);
        std::memcpy(out, tile.data.get(), static_cast<size_t>(tile.size));
        out += tile.size;
    }
    st.bytes = blob.size();
    return true;
}

bool NavBlobChecksum(const uint8_t *blob, size_t size, uint32_t *mapChecksum)
{
    if (!blob || size < sizeof(BlobHeader))
        return false;
    BlobHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (header.magic != kBlobMagic || header.version != kBlobVersion)
        return false;
    if (mapChecksum)
        *mapChecksum = header.mapChecksum;
    return true;
}

NavMesh::NavMesh() = default;

NavMesh::~NavMesh()
{
    Clear();
}

void NavMesh::Clear()
{
    if (m_query)
        dtFreeNavMeshQuery(m_query);
    if (m_mesh)
        dtFreeNavMesh(m_mesh);
    delete m_filter;
    m_query = nullptr;
    m_mesh = nullptr;
    m_filter = nullptr;
}

bool NavMesh::Load(const uint8_t *blob, size_t size)
{
    Clear();
    if (!NavBlobChecksum(blob, size, nullptr))
        return false;
    BlobHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (header.tileCount == 0 || header.maxTiles <= 0 || header.maxPolys <= 0
        || header.tileCount > static_cast<uint32_t>(header.maxTiles))
        return false;

    dtNavMeshParams nav;
    std::memset(&nav, 0, sizeof(nav));
    std::memcpy(nav.orig, header.origin, sizeof(nav.orig));
    nav.tileWidth = header.tileWidth;
    nav.tileHeight = header.tileHeight;
    nav.maxTiles = header.maxTiles;
    nav.maxPolys = header.maxPolys;

    m_mesh = dtAllocNavMesh();
    if (!m_mesh || dtStatusFailed(m_mesh->init(&nav)))
    {
        Clear();
        return false;
    }

    size_t at = sizeof(header);
    for (uint32_t i = 0; i < header.tileCount; ++i)
    {
        TileHeader th;
        if (size - at < sizeof(th))
        {
            Clear();
            return false;
        }
        std::memcpy(&th, blob + at, sizeof(th));
        at += sizeof(th);
        if (th.size == 0 || size - at < th.size)
        {
            Clear();
            return false;
        }
        unsigned char *data = static_cast<unsigned char *>(dtAlloc(th.size, DT_ALLOC_PERM));
        if (!data)
        {
            Clear();
            return false;
        }
        std::memcpy(data, blob + at, th.size);
        at += th.size;
        if (dtStatusFailed(m_mesh->addTile(data, static_cast<int>(th.size), DT_TILE_FREE_DATA, 0, nullptr)))
        {
            dtFree(data);
            Clear();
            return false;
        }
    }
    if (at != size)
    {
        Clear();
        return false;
    }

    m_query = dtAllocNavMeshQuery();
    if (!m_query || dtStatusFailed(m_query->init(m_mesh, 2048)))
    {
        Clear();
        return false;
    }
    m_filter = new dtQueryFilter();
    m_filter->setIncludeFlags(kPolyWalk);
    m_filter->setExcludeFlags(0);
    m_filter->setAreaCost(kAreaDrop, kDropCost);
    return true;
}

int NavMesh::PolygonCount() const
{
    if (!m_mesh)
        return 0;
    int count = 0;
    const dtNavMesh *mesh = m_mesh;
    for (int i = 0; i < mesh->getMaxTiles(); ++i)
    {
        const dtMeshTile *tile = mesh->getTile(i);
        if (tile && tile->header)
            count += tile->header->polyCount;
    }
    return count;
}

int NavMesh::DropLinkCount() const
{
    if (!m_mesh)
        return 0;
    int count = 0;
    const dtNavMesh *mesh = m_mesh;
    for (int i = 0; i < mesh->getMaxTiles(); ++i)
    {
        const dtMeshTile *tile = mesh->getTile(i);
        if (tile && tile->header)
            count += tile->header->offMeshConCount;
    }
    return count;
}

bool NavMesh::Nearest(const float point[3], float reach, float out[3]) const
{
    if (!m_query)
        return false;
    float center[3];
    ToRecast(point, center);
    const float extents[3] = {reach, reach * 2.0f, reach};
    dtPolyRef ref = 0;
    float nearest[3];
    if (dtStatusFailed(m_query->findNearestPoly(center, extents, m_filter, &ref, nearest)) || !ref)
        return false;
    FromRecast(nearest, out);
    return true;
}

int NavMesh::FindPath(const float start[3],
                      const float end[3],
                      float (*points)[3],
                      int maxPoints,
                      bool *complete,
                      float goalOnMesh[3]) const
{
    if (complete)
        *complete = false;
    if (!m_query || maxPoints < 2)
        return 0;
    float s[3];
    float e[3];
    ToRecast(start, s);
    ToRecast(end, e);
    const float extents[3] = {48.0f, 96.0f, 48.0f};
    dtPolyRef startRef = 0;
    dtPolyRef endRef = 0;
    float sOn[3];
    float eOn[3];
    if (dtStatusFailed(m_query->findNearestPoly(s, extents, m_filter, &startRef, sOn)) || !startRef)
        return 0;
    // The floor under the goal first: from 96 units below it to 24 above.
    const float under[3] = {e[0], e[1] - 36.0f, e[2]};
    const float underExtents[3] = {48.0f, 60.0f, 48.0f};
    if (dtStatusFailed(m_query->findNearestPoly(under, underExtents, m_filter, &endRef, eOn)) || !endRef)
    {
        endRef = 0;
        if (dtStatusFailed(m_query->findNearestPoly(e, extents, m_filter, &endRef, eOn)) || !endRef)
            return 0;
    }
    if (goalOnMesh)
        FromRecast(eOn, goalOnMesh);

    constexpr int kMaxCorridor = 512;
    dtPolyRef corridor[kMaxCorridor];
    int corridorCount = 0;
    if (dtStatusFailed(m_query->findPath(startRef, endRef, sOn, eOn, m_filter, corridor, &corridorCount, kMaxCorridor))
        || corridorCount <= 0)
        return 0;

    // A partial path ends at the reachable point closest to the goal.
    float goal[3];
    rcVcopy(goal, eOn);
    if (corridor[corridorCount - 1] != endRef)
        m_query->closestPointOnPoly(corridor[corridorCount - 1], eOn, goal, nullptr);

    std::vector<float> straight(static_cast<size_t>(maxPoints) * 3);
    int count = 0;
    if (dtStatusFailed(m_query->findStraightPath(sOn, goal, corridor, corridorCount, straight.data(), nullptr,
                                                 nullptr, &count, maxPoints))
        || count <= 0)
        return 0;
    for (int i = 0; i < count; ++i)
        FromRecast(&straight[i * 3], points[i]);
    if (complete)
        *complete = corridor[corridorCount - 1] == endRef;
    return count;
}

namespace
{
thread_local float (*s_unit)(void *) = nullptr;
thread_local void *s_unitContext = nullptr;
float UnitTrampoline()
{
    float value = s_unit ? s_unit(s_unitContext) : 0.5f;
    if (!(value >= 0.0f))
        value = 0.0f;
    if (value >= 1.0f)
        value = 0.99999f;
    return value;
}
} // namespace

bool NavMesh::RandomReachablePoint(const float from[3], float (*unit)(void *), void *context, float out[3]) const
{
    return RandomAround(from, 1.0e6f, unit, context, out);
}

bool NavMesh::RandomPointNear(const float from[3], float radius, float (*unit)(void *), void *context,
                              float out[3]) const
{
    return RandomAround(from, radius, unit, context, out);
}

float PathLength(const float (*points)[3], int count)
{
    float length = 0.0f;
    for (int i = 1; i < count; ++i)
    {
        const float dx = points[i][0] - points[i - 1][0];
        const float dy = points[i][1] - points[i - 1][1];
        const float dz = points[i][2] - points[i - 1][2];
        length += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return length;
}

bool NavMesh::RandomAround(const float from[3], float radius, float (*unit)(void *), void *context, float out[3]) const
{
    if (!m_query || !unit)
        return false;
    float s[3];
    ToRecast(from, s);
    const float extents[3] = {48.0f, 96.0f, 48.0f};
    dtPolyRef startRef = 0;
    float sOn[3];
    if (dtStatusFailed(m_query->findNearestPoly(s, extents, m_filter, &startRef, sOn)) || !startRef)
        return false;
    s_unit = unit;
    s_unitContext = context;
    dtPolyRef ref = 0;
    float point[3];
    const dtStatus status =
        m_query->findRandomPointAroundCircle(startRef, sOn, radius, m_filter, UnitTrampoline, &ref, point);
    s_unit = nullptr;
    s_unitContext = nullptr;
    if (dtStatusFailed(status) || !ref)
        return false;
    FromRecast(point, out);
    return true;
}

} // namespace kisak::bots
