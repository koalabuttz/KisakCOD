// Host test for src/bots/bot_navmesh.h (scripts/tests/test_bot_navmesh.py).
//
// Made-up rooms in game units, z up: a floor, two rooms with a door, a door
// too narrow for a player, a step and a ledge, ramps, a floor facing down,
// two rooms with no way between, platforms a player steps down from (or
// cannot: too high, a railing, a wall). The mesh must find the ways a player has
// and no others, and its cache blob must refuse anything damaged.

#include <bots/bot_navmesh.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace kb = kisak::bots;

static int gFailures = 0;

static void Check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++gFailures;
}

static float Dist2D(const float *a, const float *b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

// A solid box, every face wound so its normal points out.
static void Box(kb::NavGeometry &g, float x0, float y0, float z0, float x1, float y1, float z1)
{
    const float p[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                           {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    // Each quad counter-clockwise seen from outside.
    const int quads[6][4] = {
        {4, 5, 6, 7}, // top (+z)
        {0, 3, 2, 1}, // bottom (-z)
        {0, 1, 5, 4}, // -y
        {2, 3, 7, 6}, // +y
        {1, 2, 6, 5}, // +x
        {3, 0, 4, 7}, // -x
    };
    for (const auto &q : quads)
    {
        const float poly[4][3] = {{p[q[0]][0], p[q[0]][1], p[q[0]][2]},
                                  {p[q[1]][0], p[q[1]][1], p[q[1]][2]},
                                  {p[q[2]][0], p[q[2]][1], p[q[2]][2]},
                                  {p[q[3]][0], p[q[3]][1], p[q[3]][2]}};
        g.AddPolygon(poly, 4);
    }
}

// A floor slab: 16 units thick, top at z.
static void Floor(kb::NavGeometry &g, float x0, float y0, float x1, float y1, float z = 0.0f)
{
    Box(g, x0, y0, z - 16.0f, x1, y1, z);
}

struct Built
{
    bool ok = false;
    kb::NavMesh mesh;
    std::vector<uint8_t> blob;
    kb::NavBuildStats stats;
    char error[128] = {};
};

static void Build(const kb::NavGeometry &g, Built &out, uint32_t checksum = 1234)
{
    out.ok = kb::BuildNavMesh(g, kb::NavBuildParams(), checksum, out.blob, &out.stats, out.error, sizeof(out.error))
             && out.mesh.Load(out.blob.data(), out.blob.size());
}

static float PathLength(float (*pts)[3], int n)
{
    float total = 0.0f;
    for (int i = 1; i < n; ++i)
        total += std::sqrt((pts[i][0] - pts[i - 1][0]) * (pts[i][0] - pts[i - 1][0])
                           + (pts[i][1] - pts[i - 1][1]) * (pts[i][1] - pts[i - 1][1])
                           + (pts[i][2] - pts[i - 1][2]) * (pts[i][2] - pts[i - 1][2]));
    return total;
}

static bool Reaches(const kb::NavMesh &mesh, const float *from, const float *to, float tolerance = 24.0f)
{
    float pts[64][3];
    int n = mesh.FindPath(from, to, pts, 64);
    return n >= 2 && Dist2D(pts[n - 1], to) <= tolerance && std::fabs(pts[n - 1][2] - to[2]) <= 24.0f;
}

static void OpenFloor()
{
    std::printf("\n== an open floor ==\n");
    kb::NavGeometry g;
    Floor(g, 0, 0, 1000, 1000);
    Built b;
    Build(g, b);
    Check(b.ok, "a 1000 x 1000 floor builds and loads");
    Check(b.mesh.PolygonCount() > 0, "it has polygons");
    float a[3] = {100, 100, 0}, c[3] = {900, 900, 0};
    float pts[16][3];
    int n = b.mesh.FindPath(a, c, pts, 16);
    Check(n == 2, "corner to corner is one straight line");
    Check(n >= 2 && Dist2D(pts[n - 1], c) < 2.0f && std::fabs(pts[n - 1][2]) < 6.0f,
          "the path ends at the goal, on the floor");
    float near[3];
    float above[3] = {500, 500, 40};
    Check(b.mesh.Nearest(above, 64, near) && std::fabs(near[2]) < 6.0f && Dist2D(near, above) < 2.0f,
          "the nearest point below a standing player is on the floor");
    float edge[3] = {5, 500, 0};
    Check(b.mesh.Nearest(edge, 64, near) && near[0] >= 14.0f, "the mesh keeps a player's radius from the edge");
}

static void Doorway(float doorWidth, bool expectWay, const char *what)
{
    kb::NavGeometry g;
    Floor(g, 0, 0, 1200, 600);
    const float doorMid = 300.0f;
    // A wall across x = 600..616 with a door in it.
    Box(g, 600, 0, 0, 616, doorMid - doorWidth * 0.5f, 128);
    Box(g, 600, doorMid + doorWidth * 0.5f, 0, 616, 600, 128);
    Built b;
    Build(g, b);
    float from[3] = {300, 80, 0}, to[3] = {900, 80, 0};
    float pts[64][3];
    int n = b.ok ? b.mesh.FindPath(from, to, pts, 64) : 0;
    bool reaches = n >= 2 && Dist2D(pts[n - 1], to) < 8.0f;
    bool throughDoor = false;
    for (int i = 0; i < n; ++i)
        if (pts[i][0] > 560 && pts[i][0] < 656 && std::fabs(pts[i][1] - doorMid) < doorWidth * 0.5f)
            throughDoor = true;
    if (expectWay)
        Check(b.ok && reaches && throughDoor && PathLength(pts, n) > 600.0f, what);
    else
        Check(b.ok && !reaches, what);
}

static void Doors()
{
    std::printf("\n== doors ==\n");
    Doorway(64.0f, true, "two rooms with a 64-unit door: the way goes through the door");
    Doorway(24.0f, false, "a 24-unit gap is too narrow for a 30-unit player: no way through");
}

static void Heights()
{
    std::printf("\n== steps, ledges and ramps ==\n");
    kb::NavGeometry g;
    Floor(g, 0, 0, 1600, 1600);
    Box(g, 200, 200, 0, 500, 500, 16);   // a 16-unit step: climbable
    Box(g, 900, 200, 0, 1200, 500, 40);  // a 40-unit ledge: not without a jump
    // A 25 degree ramp from the floor up to a platform at z 120.
    const float rampRise = 120.0f;
    const float rampRun = rampRise / std::tan(25.0f * 3.14159265f / 180.0f);
    const float r0 = 300.0f;
    const float r[4][3] = {{200, 900, 0}, {200 + rampRun, 900, rampRise}, {200 + rampRun, 1100, rampRise}, {200, 1100, 0}};
    g.AddPolygon(r, 4);
    Box(g, 200 + rampRun, 900, 0, 200 + rampRun + r0, 1100, rampRise);
    // A 60 degree ramp to another platform at z 120.
    const float steepRun = rampRise / std::tan(60.0f * 3.14159265f / 180.0f);
    const float s[4][3] = {{900, 900, 0}, {900 + steepRun, 900, rampRise}, {900 + steepRun, 1100, rampRise}, {900, 1100, 0}};
    g.AddPolygon(s, 4);
    Box(g, 900 + steepRun, 900, 0, 900 + steepRun + r0, 1100, rampRise);
    Built b;
    Build(g, b);
    Check(b.ok, "the steps-and-ramps yard builds");
    float start[3] = {100, 100, 0};
    float onStep[3] = {350, 350, 16};
    float onLedge[3] = {1050, 350, 40};
    float onRampTop[3] = {200 + rampRun + 150, 1000, rampRise};
    float onSteepTop[3] = {900 + steepRun + 150, 1000, rampRise};
    Check(Reaches(b.mesh, start, onStep), "a 16-unit step is walked up");
    Check(!Reaches(b.mesh, start, onLedge), "a 40-unit ledge is not");
    Check(Reaches(b.mesh, start, onRampTop), "a 25 degree ramp is walked up");
    Check(!Reaches(b.mesh, start, onSteepTop), "a 60 degree ramp is not");
}

// A yard 1600 x 800 with a platform `height` high over its far half; its
// edge at x 800 looks over the floor. `rail` is the height of a wall along
// that edge above the platform (0 for none).
static void Platform(kb::NavGeometry &g, float height, float rail)
{
    Floor(g, 0, 0, 1600, 800);
    Box(g, 800, 0, 0, 1600, 800, height);
    if (rail > 0.0f)
        Box(g, 800, 0, height, 808, 800, height + rail);
}

static void Drops()
{
    std::printf("\n== drop-downs ==\n");
    float below[3] = {300, 400, 0};
    {
        kb::NavGeometry g;
        Platform(g, 96, 0);
        Built b;
        Build(g, b);
        std::printf("     %d drop-down(s)\n", b.stats.dropLinks);
        float top[3] = {1200, 400, 96};
        Check(b.ok && b.stats.dropLinks > 0 && b.mesh.DropLinkCount() == b.stats.dropLinks,
              "a 96-unit platform over a floor has drop-downs, and the loaded mesh holds them all");
        float pts[32][3];
        const int n = b.mesh.FindPath(top, below, pts, 32);
        bool drops = false;
        for (int i = 1; i < n; ++i)
            drops = drops || (pts[i - 1][2] - pts[i][2] > 80.0f && Dist2D(pts[i - 1], pts[i]) < 100.0f);
        Check(Reaches(b.mesh, top, below), "the way from the platform steps off it to the floor below");
        Check(drops, "with a corner at the edge and the next on the floor below it");
        Check(!Reaches(b.mesh, below, top), "but not back up: a drop-down is one way");
    }
    {
        kb::NavGeometry g;
        Platform(g, 200, 0);
        Built b;
        Build(g, b);
        float top[3] = {1200, 400, 200};
        Check(b.ok && b.stats.dropLinks == 0 && !Reaches(b.mesh, top, below),
              "a 200-unit drop would hurt: no drop-down");
    }
    {
        kb::NavGeometry g;
        Platform(g, 96, 40);
        Built b;
        Build(g, b);
        float top[3] = {1200, 400, 96};
        Check(b.ok && b.stats.dropLinks == 0 && !Reaches(b.mesh, top, below),
              "a 40-unit railing along the edge: no drop-down");
    }
    {
        kb::NavGeometry g;
        Platform(g, 96, 200);
        Built b;
        Build(g, b);
        float top[3] = {1200, 400, 96};
        Check(b.ok && b.stats.dropLinks == 0 && !Reaches(b.mesh, top, below),
              "a wall between the platform and the floor: no drop-down");
    }
    {
        // A balcony: a slab 16 thick at z 104 over the floor, 200 deep,
        // reached by nothing but the drop. The floor under it stays a floor.
        kb::NavGeometry g;
        Floor(g, 0, 0, 1600, 800);
        Box(g, 800, 0, 88, 1000, 800, 104);
        Built b;
        Build(g, b);
        float balcony[3] = {900, 400, 104};
        float under[3] = {900, 400, 0};
        Check(b.ok && Reaches(b.mesh, balcony, below), "from a balcony the way drops to the floor");
        Check(Reaches(b.mesh, below, under), "and the floor under the balcony is still walked");
    }
}

static void Storeys()
{
    std::printf("\n== a goal under a low ceiling ==\n");
    // A floor, and over its far half an upper floor whose top is 100 units
    // up (84 of clearance under it), reached by nothing but its edge.
    kb::NavGeometry g;
    Floor(g, 0, 0, 1600, 800);
    Box(g, 800, 0, 84, 1600, 800, 100);
    Built b;
    Build(g, b);
    Check(b.ok, "the two storeys build");
    float start[3] = {300, 400, 0};
    // A shot heard at a player's eye, 60 up, under the upper floor: its top
    // is closer than the shooter's own floor.
    float eye[3] = {1200, 400, 60};
    float pts[32][3];
    bool complete = false;
    float onMesh[3] = {};
    int n = b.mesh.FindPath(start, eye, pts, 32, &complete, onMesh);
    Check(n >= 2 && std::fabs(pts[n - 1][2]) < 6.0f && complete && std::fabs(onMesh[2]) < 6.0f,
          "the way to an eye under the upper floor ends on the floor under it, not upstairs");
    float chest[3] = {1200, 400, 42};
    n = b.mesh.FindPath(start, chest, pts, 32, &complete);
    Check(n >= 2 && std::fabs(pts[n - 1][2]) < 6.0f && complete, "and so does the way to a chest");
    float top[3] = {1300, 400, 100};
    n = b.mesh.FindPath(top, eye, pts, 32, &complete);
    Check(n >= 2 && std::fabs(pts[n - 1][2]) < 6.0f && complete,
          "from upstairs, the way to a player right below drops off the edge and comes back under");
    float upstairs[3] = {1200, 400, 160};
    n = b.mesh.FindPath(start, upstairs, pts, 32, &complete, onMesh);
    Check(n >= 2 && !complete && std::fabs(onMesh[2] - 100.0f) < 6.0f,
          "an eye upstairs is upstairs, and from below no way reaches it");
}

static void Facing()
{
    std::printf("\n== which side is the floor ==\n");
    kb::NavGeometry g;
    const float down[4][3] = {{0, 0, 0}, {0, 1000, 0}, {1000, 1000, 0}, {1000, 0, 0}}; // clockwise from above
    g.AddPolygon(down, 4);
    Built b;
    Build(g, b);
    Check(!b.ok, "a floor wound to face down is not walkable");
    kb::NavGeometry up;
    const float upq[4][3] = {{0, 0, 0}, {1000, 0, 0}, {1000, 1000, 0}, {0, 1000, 0}};
    up.AddPolygon(upq, 4);
    Built c;
    Build(up, c);
    Check(c.ok, "the same floor wound to face up is");
}

struct Seq
{
    unsigned int state = 99;
};
static float NextUnit(void *ctx)
{
    Seq *s = static_cast<Seq *>(ctx);
    s->state = s->state * 1664525u + 1013904223u;
    return static_cast<float>(s->state >> 8) * (1.0f / 16777216.0f);
}

static void Islands()
{
    std::printf("\n== random goals stay reachable ==\n");
    kb::NavGeometry g;
    Floor(g, 0, 0, 800, 800);
    Floor(g, 1200, 0, 2000, 800); // a second room with no way across the gap
    Built b;
    Build(g, b);
    Check(b.ok, "two separate rooms build");
    Seq seq;
    float from[3] = {400, 400, 0};
    bool allA = true;
    int got = 0;
    for (int i = 0; i < 200; ++i)
    {
        float p[3];
        if (b.mesh.RandomReachablePoint(from, NextUnit, &seq, p))
        {
            ++got;
            if (p[0] > 820.0f)
                allA = false;
        }
    }
    Check(got == 200, "every pick finds a point");
    Check(allA, "and every point is in the room the bot stands in");
    float other[3] = {1600, 400, 0};
    Check(!Reaches(b.mesh, from, other), "the other room is not reachable");
}

static void Nearby()
{
    std::printf("\n== points near the bot ==\n");
    kb::NavGeometry g;
    Floor(g, 0, 0, 4000, 4000);
    Built b;
    Build(g, b);
    Seq seq;
    float from[3] = {2000, 2000, 0};
    float nearSum = 0.0f;
    float anySum = 0.0f;
    float farthest = 0.0f;
    int got = 0;
    for (int i = 0; i < 200; ++i)
    {
        float p[3];
        if (b.mesh.RandomPointNear(from, 300.0f, NextUnit, &seq, p))
        {
            ++got;
            const float d = std::hypot(p[0] - from[0], p[1] - from[1]);
            nearSum += d;
            farthest = std::fmax(farthest, d);
        }
        if (b.mesh.RandomReachablePoint(from, NextUnit, &seq, p))
            anySum += std::hypot(p[0] - from[0], p[1] - from[1]);
    }
    std::printf("INFO: near picks: mean %.0f, farthest %.0f units; anywhere: mean %.0f\n", nearSum / 200.0f, farthest,
                anySum / 200.0f);
    Check(got == 200, "every near pick finds a point");
    // Detour keeps polygons that reach into the circle, so a point can lie a
    // polygon's size beyond it.
    Check(farthest < 1000.0f && nearSum < 0.5f * anySum, "near picks stay near");

    const float path[3][3] = {{0, 0, 0}, {300, 0, 0}, {300, 400, 0}};
    Check(std::fabs(kb::PathLength(path, 3) - 700.0f) < 0.01f && kb::PathLength(path, 1) == 0.0f,
          "a path's length is the sum of its legs");
}

static void Cache()
{
    std::printf("\n== the cache blob ==\n");
    kb::NavGeometry g;
    Floor(g, 0, 0, 1000, 1000);
    Built b;
    Build(g, b, 0xdeadbeefu);
    uint32_t sum = 0;
    Check(b.ok && kb::NavBlobChecksum(b.blob.data(), b.blob.size(), &sum) && sum == 0xdeadbeefu,
          "the blob carries the map's checksum");
    kb::NavMesh again;
    Check(again.Load(b.blob.data(), b.blob.size()) && again.PolygonCount() == b.mesh.PolygonCount(),
          "a saved blob loads to the same mesh");
    std::vector<uint8_t> cut(b.blob.begin(), b.blob.end() - 7);
    Check(!again.Load(cut.data(), cut.size()) && !again.Loaded(), "a truncated blob is refused");
    std::vector<uint8_t> longer(b.blob);
    longer.push_back(0);
    Check(!again.Load(longer.data(), longer.size()), "a blob with bytes after it is refused");
    std::vector<uint8_t> old(b.blob);
    old[4] ^= 0xff; // version
    Check(!kb::NavBlobChecksum(old.data(), old.size(), nullptr) && !again.Load(old.data(), old.size()),
          "a blob of another version is refused");
    Check(!again.Load(nullptr, 0) && !again.Load(b.blob.data(), 8), "an empty or tiny blob is refused");

    kb::NavGeometry bad;
    bad.verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    bad.tris = {0, 1, 5};
    std::vector<uint8_t> blob;
    char error[128] = {};
    Check(!kb::BuildNavMesh(bad, kb::NavBuildParams(), 0, blob, nullptr, error, sizeof(error)) && error[0],
          "a triangle naming a missing vertex is refused, with a reason");
    kb::NavGeometry empty;
    Check(!kb::BuildNavMesh(empty, kb::NavBuildParams(), 0, blob, nullptr, error, sizeof(error)),
          "no geometry is refused");
}

static void LargeMap()
{
    std::printf("\n== a map-sized yard ==\n");
    // About the size of a large CoD4 map: 8000 x 8000 with pillars.
    kb::NavGeometry g;
    Floor(g, -4000, -4000, 4000, 4000);
    for (int x = -3600; x <= 3600; x += 600)
        for (int y = -3600; y <= 3600; y += 600)
            Box(g, x - 40.0f, y - 40.0f, 0, x + 40.0f, y + 40.0f, 200);
    Built b;
    Build(g, b);
    std::printf("     tiles %d x %d, %d built, %d polygons, %zu bytes\n", b.stats.tilesX, b.stats.tilesY,
                b.stats.tilesBuilt, b.stats.polygons, b.stats.bytes);
    Check(b.ok, "it builds and loads");
    float a[3] = {-3900, -3900, 0}, c[3] = {3900, 3900, 0};
    Check(Reaches(b.mesh, a, c), "corner to corner across many tiles");
}

int main()
{
    OpenFloor();
    Doors();
    Heights();
    Drops();
    Storeys();
    Facing();
    Islands();
    Nearby();
    Cache();
    LargeMap();
    if (gFailures)
    {
        std::printf("\n%d bot navmesh check(s) failed.\n", gFailures);
        return 1;
    }
    std::printf("\nAll bot navmesh checks passed.\n");
    return 0;
}
