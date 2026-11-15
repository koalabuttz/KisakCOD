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

namespace kisak::bots
{

namespace
{

// "KBNV": KisakCOD bot navigation. Bump the version whenever the build or
// the layout changes, so every cache rebuilds.
constexpr uint32_t kBlobMagic = 0x564e424bu;
constexpr uint32_t kBlobVersion = 1;
constexpr int kMaxVertsPerPoly = 6;
constexpr unsigned short kPolyWalk = 0x01;
constexpr unsigned char kAreaGround = 1;

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
    rcPolyMesh *mesh = nullptr;
    rcPolyMeshDetail *detail = nullptr;

    ~TileWork()
    {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(compact);
        rcFreeContourSet(contours);
        rcFreePolyMesh(mesh);
        rcFreePolyMeshDetail(detail);
    }
};

// One tile: rasterise the triangles that touch it (with a border so the
// edges match the neighbours), filter, erode by the player's radius, build
// regions, contours, polygons and the detail mesh, then the Detour tile.
// Returns the tile data (dtAlloc'd) or nullptr when the tile has no floor.
unsigned char *BuildTile(rcContext &ctx,
                         const rcConfig &base,
                         const float *verts,
                         int vertCount,
                         const std::vector<int> &tris,
                         const std::vector<unsigned char> &areas,
                         int tx,
                         int ty,
                         const NavBuildParams &params,
                         int *dataSize,
                         int *polyCount)
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
        return nullptr;

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
            return nullptr;
        any = true;
    }
    (void)vertCount;
    if (!any)
        return nullptr;

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *work.solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *work.solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *work.solid);

    work.compact = rcAllocCompactHeightfield();
    if (!work.compact
        || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *work.solid, *work.compact))
        return nullptr;
    rcFreeHeightField(work.solid);
    work.solid = nullptr;

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *work.compact))
        return nullptr;
    if (!rcBuildDistanceField(&ctx, *work.compact))
        return nullptr;
    if (!rcBuildRegions(&ctx, *work.compact, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
        return nullptr;

    work.contours = rcAllocContourSet();
    if (!work.contours
        || !rcBuildContours(&ctx, *work.compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *work.contours))
        return nullptr;
    if (work.contours->nconts == 0)
        return nullptr;

    work.mesh = rcAllocPolyMesh();
    if (!work.mesh || !rcBuildPolyMesh(&ctx, *work.contours, cfg.maxVertsPerPoly, *work.mesh))
        return nullptr;
    work.detail = rcAllocPolyMeshDetail();
    if (!work.detail
        || !rcBuildPolyMeshDetail(&ctx, *work.mesh, *work.compact, cfg.detailSampleDist, cfg.detailSampleMaxError,
                                  *work.detail))
        return nullptr;
    if (work.mesh->npolys == 0)
        return nullptr;
    if (work.mesh->nverts >= 0xffff)
        return nullptr;

    for (int i = 0; i < work.mesh->npolys; ++i)
    {
        if (work.mesh->areas[i] == RC_WALKABLE_AREA)
            work.mesh->areas[i] = kAreaGround;
        work.mesh->flags[i] = kPolyWalk;
    }

    dtNavMeshCreateParams create;
    std::memset(&create, 0, sizeof(create));
    create.verts = work.mesh->verts;
    create.vertCount = work.mesh->nverts;
    create.polys = work.mesh->polys;
    create.polyAreas = work.mesh->areas;
    create.polyFlags = work.mesh->flags;
    create.polyCount = work.mesh->npolys;
    create.nvp = work.mesh->nvp;
    create.detailMeshes = work.detail->meshes;
    create.detailVerts = work.detail->verts;
    create.detailVertsCount = work.detail->nverts;
    create.detailTris = work.detail->tris;
    create.detailTriCount = work.detail->ntris;
    create.walkableHeight = params.agentHeight;
    create.walkableRadius = params.agentRadius;
    create.walkableClimb = params.agentClimb;
    create.tileX = tx;
    create.tileY = ty;
    create.tileLayer = 0;
    rcVcopy(create.bmin, work.mesh->bmin);
    rcVcopy(create.bmax, work.mesh->bmax);
    create.cs = cfg.cs;
    create.ch = cfg.ch;
    create.buildBvTree = true;

    unsigned char *data = nullptr;
    if (!dtCreateNavMeshData(&create, &data, dataSize))
        return nullptr;
    *polyCount = work.mesh->npolys;
    return data;
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

    struct BuiltTile
    {
        unsigned char *data;
        int size;
    };
    std::vector<BuiltTile> built;
    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            int size = 0;
            int polys = 0;
            unsigned char *data =
                BuildTile(ctx, cfg, verts.data(), vertCount, geometry.tris, areas, tx, ty, params, &size, &polys);
            if (!data)
                continue;
            if (polys > (1 << polyBits))
            {
                dtFree(data);
                for (BuiltTile &tile : built)
                    dtFree(tile.data);
                SetError(error, errorSize, "a tile has more polygons than its references can name");
                return false;
            }
            built.push_back({data, size});
            st.polygons += polys;
        }
    }
    st.tilesBuilt = static_cast<int>(built.size());
    if (built.empty())
    {
        SetError(error, errorSize, "no walkable floor");
        return false;
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
        std::memcpy(out, tile.data, static_cast<size_t>(tile.size));
        out += tile.size;
        dtFree(tile.data);
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

int NavMesh::FindPath(const float start[3], const float end[3], float (*points)[3], int maxPoints) const
{
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
    if (dtStatusFailed(m_query->findNearestPoly(e, extents, m_filter, &endRef, eOn)) || !endRef)
        return 0;

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
