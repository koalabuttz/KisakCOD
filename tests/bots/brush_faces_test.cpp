// Host test for CM_ShowSingleBrushCollision (src/qcommon/cm_showcollision.cpp),
// which turns a collision brush into its faces. It was only a debug drawing
// until the bots' navigation mesh (src/bots/bot_nav_world.cpp) used it on
// every player-solid brush of a map, and PC bot brain run 3 crashed in it:
//
// - its corner buffer held one point where the plane intersections write up
//   to 1024, so any brush overran the stack;
// - a corner on an axial plane (index below 6) compared against
//   sides[index - 6], the previous brush's sides, so a neighbour that shared
//   a plane let corners outside the brush through.
//
// Built with AddressSanitizer by scripts/tests/test_bot_navmesh.py.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <xanim/xanim.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

clipMap_t cm;

void track_static_alloc_internal(void *, int, const char *, int) {}
void AngleVectors(const float *, float *, float *, float *)
{
    abort();
}
char *va(const char *, ...)
{
    static char empty[1];
    return empty;
}
void Com_Error(errorParm_t, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    abort();
}
void MyAssertHandler(const char *file, int line, int, const char *, ...)
{
    printf("assert at %s:%d\n", file, line);
    abort();
}

namespace
{

struct Faces
{
    int faces = 0;
    int verts = 0;
    float lo[3] = {1e9f, 1e9f, 1e9f};
    float hi[3] = {-1e9f, -1e9f, -1e9f};
    float worstAboveSlope = -1e9f; // max of z - x over every corner
};
Faces s_faces;

void Collect(int count, float (*points)[3], const float *)
{
    ++s_faces.faces;
    s_faces.verts += count;
    for (int i = 0; i < count; ++i)
    {
        for (int k = 0; k < 3; ++k)
        {
            s_faces.lo[k] = std::fmin(s_faces.lo[k], points[i][k]);
            s_faces.hi[k] = std::fmax(s_faces.hi[k], points[i][k]);
        }
        s_faces.worstAboveSlope = std::fmax(s_faces.worstAboveSlope, points[i][2] - points[i][0]);
    }
}

const float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};

int s_failures = 0;
void Check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++s_failures;
}

} // namespace

int main()
{
    // A box: six faces of four corners, exactly its bounds.
    alignas(16) cbrush_t box = {};
    box.mins[0] = -64.0f;
    box.mins[1] = -32.0f;
    box.mins[2] = 0.0f;
    box.maxs[0] = 64.0f;
    box.maxs[1] = 32.0f;
    box.maxs[2] = 48.0f;
    s_faces = Faces();
    CM_ShowSingleBrushCollision(&box, kWhite, Collect);
    Check(s_faces.faces == 6 && s_faces.verts == 24, "a box has six faces of four corners");
    Check(s_faces.lo[0] == -64.0f && s_faces.hi[0] == 64.0f && s_faces.lo[1] == -32.0f && s_faces.hi[1] == 32.0f
              && s_faces.lo[2] == 0.0f && s_faces.hi[2] == 48.0f,
          "a box's faces span exactly its bounds");

    // A wedge: x 0..64 cut by the slope z <= x. Its side array follows six
    // sides of a neighbour that share the slope's plane, as brushes that
    // touch do in a map's side array.
    cplane_s slope = {};
    const float n = 1.0f / std::sqrt(2.0f);
    slope.normal[0] = -n;
    slope.normal[2] = n;
    slope.dist = 0.0f;
    cbrushside_t sides[7] = {};
    for (cbrushside_t &side : sides)
        side.plane = &slope;
    alignas(16) cbrush_t wedge = box;
    wedge.mins[0] = 0.0f;
    wedge.numsides = 1;
    wedge.sides = &sides[6];
    s_faces = Faces();
    CM_ShowSingleBrushCollision(&wedge, kWhite, Collect);
    Check(s_faces.faces == 6 && s_faces.verts == 24,
          "a wedge has its bottom, top, slope, end and two side faces");
    Check(s_faces.worstAboveSlope < 0.5f, "no wedge corner lies above its slope");
    Check(s_faces.lo[0] == 0.0f && s_faces.hi[0] == 64.0f && s_faces.lo[2] == 0.0f && s_faces.hi[2] == 48.0f,
          "a wedge's faces stay within its bounds");

    if (s_failures)
    {
        printf("%d brush face check(s) failed\n", s_failures);
        return 1;
    }
    printf("All brush face checks passed.\n");
    return 0;
}
