// The bots' navigation mesh for the map that is loaded (docs/bots.md).
//
// Bot_Nav() returns the mesh for the current map, building it the first time
// a bot needs it: the map's player-solid collision (brushes and terrain or
// patch triangles) becomes a triangle soup for Recast (bot_navmesh.cpp). The
// result is cached in the home folder as bots/nav/<map>.kbnav with the map's
// collision checksum, and later loads read the cache. The cache is read only
// from the home and base folders (FS_SV_*), never from a game archive or a
// server's download, and is checked before Detour sees it.
//
// Not in the mesh yet: static models' collision (props), entities (doors,
// vehicles, script models) and jumps or mantles. The bots notice when they
// are stuck and pick another way.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#include <xanim/xanim.h>

#include "bot_brain.h"
#include "bot_navmesh.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{

namespace kb = kisak::bots;

kb::NavMesh s_nav;
char s_navMap[64];
uint32_t s_navChecksum;
bool s_navTried;

// CM_ShowSingleBrushCollision hands each face of a brush to a callback with
// no context; this collects them for one brush at a time.
std::vector<float> *s_faceVerts;
std::vector<int> *s_faceCounts;

void CollectFace(int count, float (*points)[3], const float *)
{
    if (!s_faceVerts || count < 3)
        return;
    for (int i = 0; i < count; ++i)
    {
        s_faceVerts->push_back(points[i][0]);
        s_faceVerts->push_back(points[i][1]);
        s_faceVerts->push_back(points[i][2]);
    }
    s_faceCounts->push_back(count);
}

// Adds one brush's faces, each turned to face away from the brush's middle:
// the order the debug drawing produces is not known to be outward.
void AddBrush(kb::NavGeometry &geometry, const cbrush_t *brush, std::vector<float> &verts, std::vector<int> &counts)
{
    verts.clear();
    counts.clear();
    s_faceVerts = &verts;
    s_faceCounts = &counts;
    static const float kColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    CM_ShowSingleBrushCollision(brush, kColor, CollectFace);
    s_faceVerts = nullptr;
    s_faceCounts = nullptr;
    if (counts.empty())
        return;

    float middle[3] = {0.0f, 0.0f, 0.0f};
    const size_t pointCount = verts.size() / 3;
    for (size_t i = 0; i < pointCount; ++i)
    {
        middle[0] += verts[i * 3 + 0];
        middle[1] += verts[i * 3 + 1];
        middle[2] += verts[i * 3 + 2];
    }
    for (float &m : middle)
        m /= static_cast<float>(pointCount);

    size_t at = 0;
    float face[64][3];
    for (int count : counts)
    {
        if (count > 64)
        {
            at += static_cast<size_t>(count);
            continue;
        }
        float centre[3] = {0.0f, 0.0f, 0.0f};
        float normal[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < count; ++i)
        {
            const float *p = &verts[(at + i) * 3];
            const float *q = &verts[(at + (i + 1) % count) * 3];
            // Newell's normal: the polygon's facing from its winding.
            normal[0] += (p[1] - q[1]) * (p[2] + q[2]);
            normal[1] += (p[2] - q[2]) * (p[0] + q[0]);
            normal[2] += (p[0] - q[0]) * (p[1] + q[1]);
            centre[0] += p[0];
            centre[1] += p[1];
            centre[2] += p[2];
        }
        for (float &c : centre)
            c /= static_cast<float>(count);
        const float out = normal[0] * (centre[0] - middle[0]) + normal[1] * (centre[1] - middle[1])
                          + normal[2] * (centre[2] - middle[2]);
        for (int i = 0; i < count; ++i)
        {
            const int from = out >= 0.0f ? i : count - 1 - i;
            face[i][0] = verts[(at + from) * 3 + 0];
            face[i][1] = verts[(at + from) * 3 + 1];
            face[i][2] = verts[(at + from) * 3 + 2];
        }
        geometry.AddPolygon(face, count);
        at += static_cast<size_t>(count);
    }
}

// Terrain and patch collision: triangles in the collision trees' leaves.
// They carry no facing a player needs, so each is turned to face up.
void AddTerrain(kb::NavGeometry &geometry)
{
    std::vector<bool> seen(static_cast<size_t>(cm.partitionCount > 0 ? cm.partitionCount : 0), false);
    for (int i = 0; i < cm.aabbTreeCount; ++i)
    {
        const CollisionAabbTree &tree = cm.aabbTrees[i];
        if (tree.childCount)
            continue;
        if (tree.materialIndex >= cm.numMaterials
            || !(cm.materials[tree.materialIndex].contentFlags & MASK_PLAYERSOLID))
            continue;
        const int partitionIndex = tree.u.partitionIndex;
        if (partitionIndex < 0 || partitionIndex >= cm.partitionCount || seen[partitionIndex])
            continue;
        seen[partitionIndex] = true;
        const CollisionPartition &partition = cm.partitions[partitionIndex];
        for (int t = 0; t < partition.triCount; ++t)
        {
            const int tri = partition.firstTri + t;
            if (tri < 0 || tri >= cm.triCount)
                break;
            const uint16_t *index = &cm.triIndices[tri * 3];
            if (index[0] >= cm.vertCount || index[1] >= cm.vertCount || index[2] >= cm.vertCount)
                continue;
            const float *a = cm.verts[index[0]];
            const float *b = cm.verts[index[1]];
            const float *c = cm.verts[index[2]];
            const float nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if (nz >= 0.0f)
                geometry.AddTriangle(a, b, c);
            else
                geometry.AddTriangle(a, c, b);
        }
    }
}

// "maps/mp/mp_crash.d3dbsp" -> "mp_crash"; letters, digits and _ only.
void MapBaseName(const char *path, char *out, size_t size)
{
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    size_t n = 0;
    for (const char *p = base; *p && *p != '.' && n + 1 < size; ++p)
    {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out[n++] = ok ? c : '_';
    }
    out[n] = 0;
    if (!n && size > 1)
    {
        out[0] = '_';
        out[1] = 0;
    }
}

bool ReadCache(const char *path, std::vector<uint8_t> &blob)
{
    int handle = 0;
    const int length = FS_SV_FOpenFileRead(path, &handle);
    if (!handle)
        return false;
    bool ok = false;
    // A mesh for the largest maps is a few megabytes; anything far bigger is
    // not one of ours.
    if (length > 0 && length < 64 * 1024 * 1024)
    {
        blob.resize(static_cast<size_t>(length));
        ok = FS_Read(blob.data(), static_cast<uint32_t>(length), handle) == static_cast<uint32_t>(length);
    }
    FS_FCloseFile(handle);
    return ok;
}

bool WriteCache(const char *path, const std::vector<uint8_t> &blob)
{
    const int handle = FS_SV_FOpenFileWrite(path);
    if (!handle)
        return false;
    const uint32_t size = static_cast<uint32_t>(blob.size());
    const uint32_t written = FS_Write(reinterpret_cast<const char *>(blob.data()), size, handle);
    FS_FCloseFile(handle);
    return written == size;
}

void LoadOrBuild(const char *mapName, uint32_t checksum)
{
    char path[128];
    Com_sprintf(path, sizeof(path), "bots/nav/%s.kbnav", mapName);

    std::vector<uint8_t> blob;
    uint32_t cached = 0;
    if (ReadCache(path, blob) && kb::NavBlobChecksum(blob.data(), blob.size(), &cached) && cached == checksum
        && s_nav.Load(blob.data(), blob.size()))
    {
        Com_Printf(CON_CHANNEL_SERVER, "bots: navigation mesh for %s loaded from %s (%d polygons, %d KB)\n", mapName,
                   path, s_nav.PolygonCount(), static_cast<int>(blob.size() / 1024));
        return;
    }

    const uint32_t started = Sys_Milliseconds();
    kb::NavGeometry geometry;
    std::vector<float> faceVerts;
    std::vector<int> faceCounts;
    int brushes = 0;
    for (uint32_t i = 0; i < cm.numBrushes; ++i)
    {
        const cbrush_t *brush = &cm.brushes[i];
        if (!(brush->contents & MASK_PLAYERSOLID))
            continue;
        AddBrush(geometry, brush, faceVerts, faceCounts);
        ++brushes;
    }
    AddTerrain(geometry);
    const uint32_t gathered = Sys_Milliseconds();

    kb::NavBuildStats stats;
    char error[128] = {};
    if (!kb::BuildNavMesh(geometry, kb::NavBuildParams(), checksum, blob, &stats, error, sizeof(error))
        || !s_nav.Load(blob.data(), blob.size()))
    {
        Com_PrintWarning(CON_CHANNEL_SERVER, "bots: no navigation mesh for %s: %s\n", mapName,
                         error[0] ? error : "it did not load");
        s_nav.Clear();
        return;
    }
    const uint32_t built = Sys_Milliseconds();
    const bool saved = WriteCache(path, blob);
    Com_Printf(CON_CHANNEL_SERVER,
               "bots: navigation mesh for %s built in %.1f s (geometry %.1f s: %d brushes, %d triangles; "
               "%d x %d tiles, %d built, %d polygons, %d KB)%s%s\n",
               mapName, (built - started) * 0.001f, (gathered - started) * 0.001f, brushes, stats.inputTriangles,
               stats.tilesX, stats.tilesY, stats.tilesBuilt, stats.polygons, static_cast<int>(stats.bytes / 1024),
               saved ? ", saved to " : ", not saved", saved ? path : "");
}

} // namespace

const kisak::bots::NavMesh *Bot_Nav()
{
    if (!cm.isInUse || !cm.name)
        return nullptr;
    char mapName[64];
    MapBaseName(cm.name, mapName, sizeof(mapName));
    if (s_navTried && s_navChecksum == cm.checksum && !std::strcmp(s_navMap, mapName))
        return s_nav.Loaded() ? &s_nav : nullptr;

    s_nav.Clear();
    s_navTried = true;
    s_navChecksum = cm.checksum;
    I_strncpyz(s_navMap, mapName, sizeof(s_navMap));
    LoadOrBuild(mapName, cm.checksum);
    return s_nav.Loaded() ? &s_nav : nullptr;
}
