// See r_dpvs_static_vec.h. Nothing of the engine is included here.
#include "r_dpvs_static_vec.h"

#if KISAK_DPVS_VEC

#include <cstring>

#if defined(__arm__) && !defined(__ARM_NEON)
#error "r_dpvs_static_vec.cpp is built for NEON on 32-bit Arm (-mfpu=neon): without it this is slower than the scalar code"
#endif

namespace dpvs_vec
{
namespace
{
typedef float F4 __attribute__((vector_size(16)));
typedef int32_t I4 __attribute__((vector_size(16)));

// Four planes: a coefficient of each in one vector, and for each axis whether
// the plane's first corner is the box's maxs (all bits) or its mins (none).
struct alignas(16) Four
{
    float a[4], b[4], c[4], d[4];
    int32_t maxX[4], maxY[4], maxZ[4];
};

// A cell's planes. Plane i is lane i % 4 of fours[i / 4] and bit i of the
// masks that say which planes are still to test.
struct Planes
{
    Four fours[MAX_PLANES / 4];
};

// "No plane is left" is 0, so "the box is outside" needs a value no mask has.
constexpr uint32_t OUTSIDE = 0x80000000u;

inline __attribute__((always_inline)) F4 Load(const float *from)
{
    F4 v;
    std::memcpy(&v, from, sizeof(v));
    return v;
}

inline __attribute__((always_inline)) I4 LoadMask(const int32_t *from)
{
    I4 v;
    std::memcpy(&v, from, sizeof(v));
    return v;
}

inline __attribute__((always_inline)) F4 Lane(F4 v, int lane)
{
    return F4{v[lane], v[lane], v[lane], v[lane]};
}

// a where the mask is set, b where it is not.
inline __attribute__((always_inline)) F4 Pick(I4 mask, F4 a, F4 b)
{
    return (F4)(((I4)a & mask) | ((I4)b & ~mask));
}

// A box's six floats as two overlapping loads, then one vector a coordinate.
struct Box
{
    F4 minX, minY, minZ, maxX, maxY, maxZ;

    explicit Box(const float *bounds)
    {
        const F4 low = Load(bounds);      // mins x y z, maxs x
        const F4 high = Load(bounds + 2); // mins z, maxs x y z
        minX = Lane(low, 0);
        minY = Lane(low, 1);
        minZ = Lane(low, 2);
        maxX = Lane(high, 1);
        maxY = Lane(high, 2);
        maxZ = Lane(high, 3);
    }
};

void SetPlane(Planes *planes, uint32_t index, const Plane &plane)
{
    Four &four = planes->fours[index / 4];
    const uint32_t lane = index % 4;
    four.a[lane] = plane.coeffs[0];
    four.b[lane] = plane.coeffs[1];
    four.c[lane] = plane.coeffs[2];
    four.d[lane] = plane.coeffs[3];
    four.maxX[lane] = plane.side[0] ? -1 : 0;
    four.maxY[lane] = plane.side[1] != 4 ? -1 : 0;
    four.maxZ[lane] = plane.side[2] != 8 ? -1 : 0;
}

// The planes of the mask moved together: plane i of `to` is the mask's i-th.
// Returns how many there are.
uint32_t MoveTogether(const Planes &from, uint32_t mask, Planes *to)
{
    uint32_t count = 0;
    for (uint32_t index = 0; mask >> index; ++index)
    {
        if (!((mask >> index) & 1u))
            continue;
        const Four &in = from.fours[index / 4];
        Four &out = to->fours[count / 4];
        const uint32_t inLane = index % 4, outLane = count % 4;
        out.a[outLane] = in.a[inLane];
        out.b[outLane] = in.b[inLane];
        out.c[outLane] = in.c[inLane];
        out.d[outLane] = in.d[inLane];
        out.maxX[outLane] = in.maxX[inLane];
        out.maxY[outLane] = in.maxY[inLane];
        out.maxZ[outLane] = in.maxZ[inLane];
        ++count;
    }
    // The lanes of the last four that hold no plane are never in a mask, but
    // they are summed: give them numbers.
    for (uint32_t index = count; index % 4; ++index)
    {
        Four &out = to->fours[index / 4];
        const uint32_t lane = index % 4;
        out.a[lane] = out.b[lane] = out.c[lane] = out.d[lane] = 0.0f;
        out.maxX[lane] = out.maxY[lane] = out.maxZ[lane] = 0;
    }
    return count;
}

// How many fours the mask's planes are in, and how many they would fill.
inline uint32_t FoursIn(uint32_t mask)
{
    return ((mask & 0x000fu) != 0) + ((mask & 0x00f0u) != 0) + ((mask & 0x0f00u) != 0) + ((mask & 0xf000u) != 0);
}

inline uint32_t FoursFor(uint32_t mask)
{
    return ((uint32_t)__builtin_popcount(mask) + 3u) / 4u;
}

// A four's vectors, loaded where they are summed. (Kept in registers across
// a loop of boxes they would crowd out the box's own six on 32-bit Arm, and
// the compiler would put them back on the stack.)
struct Loaded
{
    F4 a, b, c, d;
    I4 maxX, maxY, maxZ;

    explicit Loaded(const Four &four)
        : a(Load(four.a)), b(Load(four.b)), c(Load(four.c)), d(Load(four.d)), maxX(LoadMask(four.maxX)),
          maxY(LoadMask(four.maxY)), maxZ(LoadMask(four.maxZ))
    {
    }
};

// The lanes of a vector, each already cut down to its own bits, as one word.
inline __attribute__((always_inline)) uint32_t Bits(I4 lanes)
{
    return (uint32_t)(lanes[0] | lanes[1] | lanes[2] | lanes[3]);
}

// The tree node's two sums for four planes: the corner furthest along each
// plane (at or behind the plane: the whole box is outside), then the opposite
// corner (behind the plane: the plane cuts the box, and what is in the box
// has to be held against it too). Bits 0..3: outside. Bits 4..7: cut.
inline __attribute__((always_inline)) uint32_t NodeSums(const Loaded &four, const Box &box)
{
    const F4 first = ((Pick(four.maxX, box.maxX, box.minX) * four.a + four.d) +
                      Pick(four.maxY, box.maxY, box.minY) * four.b) +
                     Pick(four.maxZ, box.maxZ, box.minZ) * four.c;
    const F4 second = ((Pick(four.maxX, box.minX, box.maxX) * four.a + four.d) +
                       Pick(four.maxY, box.minY, box.maxY) * four.b) +
                      Pick(four.maxZ, box.minZ, box.maxZ) * four.c;
    return Bits(((first <= 0.0f) & I4{1, 2, 4, 8}) | ((second < 0.0f) & I4{16, 32, 64, 128}));
}

// A static model's or a surface's one sum for four planes. Bits 0..3: a plane
// has the whole box at or behind it.
inline __attribute__((always_inline)) uint32_t ItemSums(const Loaded &four, const Box &box)
{
    const F4 first = ((Pick(four.maxX, box.maxX, box.minX) * four.a + four.d) +
                      Pick(four.maxY, box.maxY, box.minY) * four.b) +
                     Pick(four.maxZ, box.maxZ, box.minZ) * four.c;
    return Bits((first <= 0.0f) & I4{1, 2, 4, 8});
}

// A tree node against the planes of the mask. Deep in a tree the planes left
// are nearly always in the first four alone. Returns OUTSIDE or the planes
// that cut the box.
inline __attribute__((always_inline)) uint32_t HoldNode(const Planes &planes, uint32_t active, const float *bounds)
{
    const Box box(bounds);
    uint32_t cut = 0;
    if (const uint32_t lanes = active & 15u)
    {
        const uint32_t both = NodeSums(Loaded(planes.fours[0]), box);
        if (both & lanes)
            return OUTSIDE;
        cut = (both >> 4) & lanes;
    }
    for (uint32_t shift = 4; active >> shift; shift += 4)
    {
        const uint32_t lanes = (active >> shift) & 15u;
        if (!lanes)
            continue;
        const uint32_t both = NodeSums(Loaded(planes.fours[shift / 4]), box);
        if (both & lanes)
            return OUTSIDE;
        cut |= ((both >> 4) & lanes) << shift;
    }
    return cut;
}

// A static model or a surface against the planes of the mask: true when one
// of them has the whole box at or behind it.
inline __attribute__((always_inline)) bool IsOutside(const Planes &planes, uint32_t active, const float *bounds)
{
    const Box box(bounds);
    if ((active & 15u) && (ItemSums(Loaded(planes.fours[0]), box) & active))
        return true;
    for (uint32_t shift = 4; active >> shift; shift += 4)
    {
        const uint32_t lanes = (active >> shift) & 15u;
        if (lanes && (ItemSums(Loaded(planes.fours[shift / 4]), box) & lanes))
            return true;
    }
    return false;
}

inline const float *At(const float *first, uint32_t stride, uint32_t index)
{
    return reinterpret_cast<const float *>(reinterpret_cast<const char *>(first) + (size_t)stride * index);
}

// What a node holds: its static models, and its surfaces as a run of places
// in the world's sorted list. Nothing of a kind that is not drawn.
struct Contents
{
    const uint16_t *models;
    uint32_t modelCount;
    const uint16_t *surfaces;
    uint32_t surfaceCount;

    Contents(const Tree *tree, const World &world)
        : models(tree->smodelIndexes), modelCount(world.modelVis ? tree->smodelIndexCount : 0),
          surfaces(world.sortedSurfIndex + (world.decals ? tree->startSurfIndex : tree->startSurfIndexNoDecal)),
          surfaceCount(!world.surfaceVis ? 0 : world.decals ? tree->surfaceCount : tree->surfaceCountNoDecal)
    {
    }
};

// The box of a static model or surface that is about to be held: it may lie
// over two cache lines.
inline __attribute__((always_inline)) void AskFor(const float *bounds)
{
    __builtin_prefetch(bounds);
    __builtin_prefetch(reinterpret_cast<const char *>(bounds) + 23);
}

// A leaf that planes cut: each static model and surface in it that is not
// marked yet, on its own. Their boxes lie anywhere in the world's lists, so
// with World::fetchAhead the box two ahead is asked for meanwhile (one that
// is marked already is not read).
__attribute__((noinline)) void HoldLeaf(const Tree *tree, const Planes &planes, uint32_t active, const World &world)
{
    enum
    {
        AHEAD = 2
    };
    const Contents in(tree, world);
    const bool ahead = world.fetchAhead;
    for (uint32_t i = 0; i < in.modelCount; ++i)
    {
        if (ahead && i + AHEAD < in.modelCount && !world.modelVis[in.models[i + AHEAD]])
            AskFor(At(world.modelBounds, world.modelStride, in.models[i + AHEAD]));
        const uint32_t index = in.models[i];
        if (!world.modelVis[index] &&
            !IsOutside(planes, active, At(world.modelBounds, world.modelStride, index)))
            world.modelVis[index] = 1;
    }
    for (uint32_t i = 0; i < in.surfaceCount; ++i)
    {
        if (ahead && i + AHEAD < in.surfaceCount && !world.surfaceVis[in.surfaces[i + AHEAD]])
            AskFor(At(world.surfaceBounds, world.surfaceStride, in.surfaces[i + AHEAD]));
        const uint32_t index = in.surfaces[i];
        if (!world.surfaceVis[index] &&
            !IsOutside(planes, active, At(world.surfaceBounds, world.surfaceStride, index)))
            world.surfaceVis[index] = 1;
    }
}

// The nodes next to each other in the tree (a node's children, or the root
// alone), each against the planes of the mask. Most are outside or small, so
// they are held in this one loop, and a call is made only to go down.
void Walk(const Tree *nodes, uint32_t nodeCount, const Planes &planes, uint32_t active, const World &world)
{
    for (uint32_t node = 0; node < nodeCount; ++node)
    {
        const Tree *const tree = &nodes[node];
        // World::fetchAhead: the node two further in the row, both of its
        // cache lines.
        if (world.fetchAhead && node + 2 < nodeCount)
        {
            __builtin_prefetch(tree + 2);
            __builtin_prefetch(reinterpret_cast<const char *>(tree + 3) - 1);
        }
        const uint32_t cut = HoldNode(planes, active, tree->mins);
        if (cut == OUTSIDE)
            continue;

        if (!cut)
        {
            // No plane cuts the box: everything in it is in.
            const Contents in(tree, world);
            for (uint32_t i = 0; i < in.modelCount; ++i)
                world.modelVis[in.models[i]] = 1;
            for (uint32_t i = 0; i < in.surfaceCount; ++i)
                world.surfaceVis[in.surfaces[i]] = 1;
            continue;
        }

        // Fewer fours to sum for everything below, when the planes left fit.
        Planes together;
        const Planes *below = &planes;
        uint32_t belowActive = cut;
        if (FoursFor(cut) < FoursIn(cut) || !(cut & 15u))
        {
            belowActive = (1u << MoveTogether(planes, cut, &together)) - 1u;
            below = &together;
        }

        if (tree->childCount)
            Walk(reinterpret_cast<const Tree *>(reinterpret_cast<const char *>(tree) + tree->childrenOffset),
                 tree->childCount, *below, belowActive, world);
        else
            HoldLeaf(tree, *below, belowActive, world);
    }
}
} // namespace

bool AddTree(const Tree *tree, const Plane *planes, uint32_t planeCount, const World &world)
{
    if (!planeCount || planeCount > MAX_PLANES)
        return false;
    Planes all = {};
    for (uint32_t index = 0; index < planeCount; ++index)
    {
        const Plane &plane = planes[index];
        // side[] says which of the box's six floats a sum reads. Any plane
        // of the engine names the mins or the maxs of its own axis; one that
        // does not is left to the scalar code, which reads what it names.
        if ((plane.side[0] != 0 && plane.side[0] != 12) || (plane.side[1] != 4 && plane.side[1] != 16) ||
            (plane.side[2] != 8 && plane.side[2] != 20))
            return false;
        SetPlane(&all, index, plane);
    }
    Walk(tree, 1, all, (1u << planeCount) - 1u, world);
    return true;
}
} // namespace dpvs_vec
#endif
