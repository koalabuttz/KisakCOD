#pragma once

// The static culling walk of a cell's box tree, four planes at a time.
//
// R_AddAabbTreeSurfacesInFrustum_r (r_dpvs_static.cpp) holds a box against
// its clip planes one plane at a time: two sums for a tree node, one for each
// static model and surface of a leaf, every sum reading its three box
// coordinates through the plane's side[] offsets. Here a cell's planes are
// laid out in fours and a box is held against four of them with one sum. The
// planes still to test go down the tree as a mask of sixteen bits instead of
// a list, and are moved together into fewer fours when that leaves a four
// out.
//
// Every sum is ((corner x * a + d) + corner y * b) + corner z * c with the
// corners the scalar code picks, in its order, so the two make the same
// decisions and mark the same models and surfaces (as long as no multiply-add
// is fused, which the build's -ffp-contract=off sees to). One difference is
// left on 32-bit Arm: NEON takes a denormal for zero. What is marked here is
// drawn and nothing else, so that is allowed (docs/linux-armv7.md).
// scripts/tests/test_static_cull_vec.py holds the two against each other.
//
// It is written with the compiler's own vector types, not with one CPU's
// intrinsics: NEON on Arm, SSE on x86. This header and r_dpvs_static_vec.cpp
// include nothing of the engine: on the Vita that one file is compiled for
// NEON (the rest of the engine is not), and no shared inline function should
// come out of it with other code than everywhere else. The engine's types are
// checked against the ones below in r_dpvs_static.cpp.

#include <cstdint>

// GCC and Clang have the vector types. On 32-bit Arm the file has to be built
// for NEON to be worth it, and only the Vita's build does that
// (scripts/platform/vita/mp_client.cmake).
#if defined(__GNUC__) && (!defined(__arm__) || defined(__vita__))
#define KISAK_DPVS_VEC 1
#else
#define KISAK_DPVS_VEC 0
#endif

#if KISAK_DPVS_VEC
namespace dpvs_vec
{
// DpvsPlane.
struct Plane
{
    float coeffs[4];
    uint8_t side[3]; // 0, 4, 8 for the box's mins, 12, 16, 20 for its maxs
    uint8_t pad;
};

// GfxAabbTree.
struct Tree
{
    float mins[3];
    float maxs[3];
    uint16_t childCount;
    uint16_t surfaceCount;
    uint16_t startSurfIndex;
    uint16_t surfaceCountNoDecal;
    uint16_t startSurfIndexNoDecal;
    uint16_t smodelIndexCount;
    uint16_t *smodelIndexes;
    int childrenOffset;
};

// What the walk reads of the world and where it marks.
struct World
{
    // The mins of static model 0 and of surface 0, each followed by its maxs;
    // the next model's or surface's are a stride of bytes further.
    const float *modelBounds;
    const float *surfaceBounds;
    uint32_t modelStride;
    uint32_t surfaceStride;
    const uint16_t *sortedSurfIndex;
    // A byte for each static model and surface, set to 1 for the ones the
    // planes do not rule out. Null: that kind is not drawn.
    uint8_t *modelVis;
    uint8_t *surfaceVis;
    bool decals; // the tree's surface ranges with decals, or without
};

enum
{
    MAX_PLANES = 16
};

// Marks the static models and surfaces of the tree that no plane rules out.
// False, with nothing marked, when it is not for this code: no plane or more
// than MAX_PLANES, or a plane whose side[] does not name the mins or the maxs
// of its own axis (the scalar code reads whatever side[] names).
bool AddTree(const Tree *tree, const Plane *planes, uint32_t planeCount, const World &world);
} // namespace dpvs_vec
#endif
