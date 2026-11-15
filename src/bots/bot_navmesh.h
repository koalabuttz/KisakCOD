// The bots' navigation mesh: where a player can walk on a map, and the way
// from one place to another (docs/bots.md).
//
// Built with Recast from the map's collision geometry (the triangles a player
// collides with, gathered by src/bots/bot_nav_world.cpp) and queried with
// Detour (deps/recastnavigation, zlib licence). The mesh is derived from the
// map, so it is built on the player's machine and cached in the home folder;
// it is never committed.
//
// Coordinates here are the game's: x and y across, z up, in game units
// (inches). Recast works y-up; the conversion is a rotation about x, which
// keeps every triangle's winding, so a floor whose triangle turns
// counter-clockwise seen from above (normal +z) stays walkable.
//
// No engine call and no engine type: scripts/tests/test_bot_navmesh.py builds
// meshes from made-up rooms on the host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class dtQueryFilter;

namespace kisak::bots
{

// A player: mins (-15, -15, 0), maxs (15, 15, 70) standing; steps up 18
// units (the stepsize); walks on slopes whose normal has z of at least 0.7.
struct NavBuildParams
{
    float cellSize = 6.0f;
    float cellHeight = 3.0f;
    float agentRadius = 15.0f;
    float agentHeight = 70.0f;
    float agentClimb = 18.0f;
    float maxSlopeDegrees = 45.0f;
    int tileCells = 64;
    // Polygons smaller than this many cells (about 2 m^2 at 6 units) are
    // dropped: ledges and the tops of props nobody can reach.
    int minRegionCells = 64;
    int mergeRegionCells = 400;
};

struct NavBuildStats
{
    int inputTriangles = 0;
    int tilesX = 0;
    int tilesY = 0;
    int tilesBuilt = 0;
    int polygons = 0;
    size_t bytes = 0;
};

// A triangle soup in game coordinates. A triangle is walkable on the side its
// winding faces: (v1 - v0) x (v2 - v0) must point up for a floor.
struct NavGeometry
{
    std::vector<float> verts; // x, y, z per vertex
    std::vector<int> tris;    // three vertex indices per triangle

    void AddTriangle(const float *a, const float *b, const float *c);
    // A convex polygon, fanned in the order given.
    void AddPolygon(const float (*points)[3], int count);
    int TriangleCount() const { return static_cast<int>(tris.size() / 3); }
};

// Builds the mesh and returns it as one blob (header + Detour tiles) that
// NavMesh::Load takes and a cache file can hold. `mapChecksum` is stored to
// tell a stale cache from a good one. Returns false with `error` set.
bool BuildNavMesh(const NavGeometry &geometry,
                  const NavBuildParams &params,
                  uint32_t mapChecksum,
                  std::vector<uint8_t> &blob,
                  NavBuildStats *stats,
                  char *error,
                  size_t errorSize);

// The checksum a cache blob was built for, or false when the blob is not a
// navigation mesh of this version.
bool NavBlobChecksum(const uint8_t *blob, size_t size, uint32_t *mapChecksum);

class NavMesh
{
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh &) = delete;
    NavMesh &operator=(const NavMesh &) = delete;

    bool Load(const uint8_t *blob, size_t size);
    void Clear();
    bool Loaded() const { return m_mesh != nullptr; }
    int PolygonCount() const;

    // The closest point on the mesh within `reach` across and twice that up
    // and down. False when there is none.
    bool Nearest(const float point[3], float reach, float out[3]) const;

    // A path from `start` to `end` as corner points, the first `start` (on the
    // mesh) and the last `end` or the closest reachable point to it. Returns
    // the number of points, 0 when there is no path.
    int FindPath(const float start[3], const float end[3], float (*points)[3], int maxPoints) const;

    // A random point on the mesh reachable from `from` (on the polygon graph),
    // picked with `unit`, a source of numbers in [0, 1).
    bool RandomReachablePoint(const float from[3], float (*unit)(void *), void *context, float out[3]) const;

    // A random point on the mesh reachable from `from`, on a polygon within
    // about `radius` of it.
    bool RandomPointNear(const float from[3], float radius, float (*unit)(void *), void *context, float out[3]) const;

private:
    bool RandomAround(const float from[3], float radius, float (*unit)(void *), void *context, float out[3]) const;

    dtNavMesh *m_mesh = nullptr;
    dtNavMeshQuery *m_query = nullptr;
    dtQueryFilter *m_filter = nullptr;
};

// The length of a path's corners, end to end.
float PathLength(const float (*points)[3], int count);

} // namespace kisak::bots
