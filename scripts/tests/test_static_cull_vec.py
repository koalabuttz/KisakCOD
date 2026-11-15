#!/usr/bin/env python3
"""Check that the four-planes-at-a-time static culling marks what the scalar code marks.

R_AddAabbTreeSurfacesInFrustum_r (src/gfx_d3d/r_dpvs_static.cpp) walks a cell's
box tree and marks the static models and surfaces that the cell's clip planes
do not rule out, one plane at a time. dpvs_vec::AddTree
(src/gfx_d3d/r_dpvs_static_vec.cpp) does the same walk with four planes to a
sum. This check compiles both on the host -- the scalar function and the
engine's structures are cut out of the production sources, not copied here --
and runs them over made-up trees and plane sets. Every byte the two mark has to
be the same:

  * boxes and planes on small whole numbers, where sums come out at exactly
    zero and "at the plane" and "behind the plane" part ways;
  * planes through a box's corner give or take the last bits, where the side
    of zero a sum lands on depends on the order it is made in;
  * planes with zero, denormal, huge, infinite and NaN coefficients, and boxes
    with such corners;
  * one to sixteen planes, so that the planes left over deep in a tree sit in
    every arrangement of the fours;
  * trees whose boxes do not hold their contents, static models listed in
    several leaves, models and surfaces already marked, decals on and off,
    models or surfaces not drawn;
  * a plane whose side[] is not one the engine makes: the vector code has to
    refuse it and mark nothing.

The scenes are the same on every host, and so is the count of what is marked:
an Arm host, where the vector code is NEON, has to mark what an x86 host marks.

--bench also prints how long the two take on a made-up scene. No game assets
are needed.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def block(source: str, start: int) -> str:
    """Return a brace-delimited declaration/function, ignoring quoted braces."""
    opening = source.index("{", start)
    depth = 1
    tokens = re.compile(
        r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''',
        re.DOTALL,
    )
    for token in tokens.finditer(source, opening + 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise ValueError("Unterminated production declaration")


def declaration(source: str, head: str) -> str:
    """The production struct that starts with this text, with its closing semicolon."""
    return block(source, source.index(head)) + ";\n"


HARNESS_TOP = r"""
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "r_dpvs_static_vec.h"

typedef uint8_t byte;
typedef uint8_t _BYTE;
typedef uint32_t _DWORD;

// What the scalar walk reads besides its tree and planes.
struct Material;
struct DebugGlobals { int boxes; };
struct dvar_t { struct { int integer; bool enabled; } current; };
static dvar_t s_showAabbTrees, s_drawDecals;
static const dvar_t *r_showAabbTrees = &s_showAabbTrees;
static const dvar_t *r_drawDecals = &s_drawDecals;
static struct { DebugGlobals debugGlobals; } s_frontEndData, *frontEndDataOut = &s_frontEndData;
static const float colorOrange[4] = {}, colorYellow[4] = {}, colorGreen[4] = {};
static void R_AddDebugBox(DebugGlobals *globals, const float *, const float *, const float *) { ++globals->boxes; }
static struct { bool drawSModels; bool drawWorld; } rg;
static thread_local byte *g_smodelVisData;
static thread_local byte *g_surfaceVisData;
union GfxColor { uint32_t packed; uint8_t array[4]; };
"""

HARNESS_WORLD = r"""
// r_dpvs_stats (r_dpvs_stats.h): the scalar walk counts what it does when asked.
static std::atomic<bool> s_dpvsCounting{false};
static uint64_t s_dpvsCounted;
static void DpvsCountAdd(DpvsCount, uint32_t count) { s_dpvsCounted += count; }

static struct
{
    struct World
    {
        struct
        {
            uint16_t *sortedSurfIndex;
            GfxStaticModelInst *smodelInsts;
            GfxSurface *surfaces;
        } dpvs;
    } *world;
} rgp;
"""

HARNESS_BOTTOM = r"""
static void check(bool ok, const std::string &message)
{
    if (!ok)
        throw std::runtime_error(message);
}

// The two have to agree on what the engine's structures are.
static_assert(sizeof(dpvs_vec::Plane) == sizeof(DpvsPlane), "DpvsPlane");
static_assert(offsetof(dpvs_vec::Plane, side) == offsetof(DpvsPlane, side), "DpvsPlane::side");
static_assert(sizeof(dpvs_vec::Tree) == sizeof(GfxAabbTree), "GfxAabbTree");
static_assert(offsetof(dpvs_vec::Tree, maxs) == offsetof(GfxAabbTree, maxs), "GfxAabbTree::maxs");
static_assert(offsetof(dpvs_vec::Tree, childCount) == offsetof(GfxAabbTree, childCount), "GfxAabbTree::childCount");
static_assert(offsetof(dpvs_vec::Tree, startSurfIndex) == offsetof(GfxAabbTree, startSurfIndex), "startSurfIndex");
static_assert(offsetof(dpvs_vec::Tree, startSurfIndexNoDecal) == offsetof(GfxAabbTree, startSurfIndexNoDecal),
              "startSurfIndexNoDecal");
static_assert(offsetof(dpvs_vec::Tree, smodelIndexes) == offsetof(GfxAabbTree, smodelIndexes), "smodelIndexes");
static_assert(offsetof(dpvs_vec::Tree, childrenOffset) == offsetof(GfxAabbTree, childrenOffset), "childrenOffset");

static uint32_t s_state = 0x2545f491u;

static uint32_t next()
{
    // xorshift32: the same sequence on every compiler and library.
    s_state ^= s_state << 13;
    s_state ^= s_state >> 17;
    s_state ^= s_state << 5;
    return s_state;
}

static uint32_t below(uint32_t limit) { return next() % limit; }

static float fromBits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

enum Numbers
{
    WHOLE,   // small whole numbers: sums are exact and often zero
    COARSE,  // multiples of 1/8 up to a few thousand, a map's sizes
    ANY,     // any bits at all: denormals, infinities, NaNs, 1e38
    NUMBERS
};

static float number(Numbers kind)
{
    switch (kind)
    {
    case WHOLE:
        return (float)((int)below(9) - 4);
    case COARSE:
        return (float)((int)below(65536) - 32768) * 0.125f;
    default:
        break;
    }
    switch (below(8))
    {
    case 0: return fromBits(next());
    case 1:  // denormal
    {
        const uint32_t fraction = below(1u << 23);
        return fromBits(fraction | (next() & 0x80000000u));
    }
    case 2: return 0.0f;
    case 3: return -0.0f;
    case 4: return below(2) ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    case 5: return std::numeric_limits<float>::quiet_NaN();
    case 6: return fromBits(0x00800000u | (next() & 0x80000000u));  // the smallest normal
    default: return (float)((int)below(2001) - 1000) * 1.0e35f;
    }
}

struct Scene
{
    std::vector<GfxStaticModelInst> models;
    std::vector<GfxSurface> surfaces;
    std::vector<uint16_t> sorted;        // a place in the tree's ranges -> a surface
    std::vector<GfxAabbTree> nodes;      // node 0 is the root; a node's children are next to each other
    std::vector<std::vector<uint16_t>> modelLists;  // one for each node
    std::vector<byte> modelVis, surfaceVis;
};

static void randomBox(Numbers kind, float *mins, float *maxs)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        const float a = number(kind);
        const float b = number(kind);
        // Mostly mins below maxs, as boxes are; now and then not, which the walks must also agree on.
        const bool flip = below(16) == 0;
        const bool swap = (a > b) != flip;
        mins[axis] = swap ? b : a;
        maxs[axis] = swap ? a : b;
    }
}

static void grow(const float *mins, const float *maxs, float *intoMins, float *intoMaxs)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        if (mins[axis] < intoMins[axis])
            intoMins[axis] = mins[axis];
        if (maxs[axis] > intoMaxs[axis])
            intoMaxs[axis] = maxs[axis];
    }
}

// A node over the surface places [first, first + count) and the given models.
// honest: its box holds what is in it. Otherwise the box is anything.
static void build(Scene &scene, size_t node, uint32_t first, uint32_t count, std::vector<uint16_t> models,
                  uint32_t depth, Numbers kind, bool honest)
{
    {
        GfxAabbTree &tree = scene.nodes[node];
        tree.startSurfIndex = (uint16_t)first;
        tree.surfaceCount = (uint16_t)count;
        // The range without decals is a part of the range with them.
        const uint32_t without = count ? below(count + 1) : 0;
        tree.startSurfIndexNoDecal = (uint16_t)(first + (count - without ? below(count - without + 1) : 0));
        tree.surfaceCountNoDecal = (uint16_t)without;
        if (honest)
        {
            float mins[3] = {1e30f, 1e30f, 1e30f}, maxs[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t i = 0; i < count; ++i)
            {
                const GfxSurface &surface = scene.surfaces[scene.sorted[first + i]];
                grow(surface.bounds[0], surface.bounds[1], mins, maxs);
            }
            for (uint16_t model : models)
                grow(scene.models[model].mins, scene.models[model].maxs, mins, maxs);
            if (!count && models.empty())
                randomBox(kind, mins, maxs);
            memcpy(tree.mins, mins, sizeof(mins));
            memcpy(tree.maxs, maxs, sizeof(maxs));
        }
        else
        {
            randomBox(kind, tree.mins, tree.maxs);
        }
        scene.modelLists[node] = models;
    }

    const uint32_t childCount = depth && (count > 1 || models.size() > 1) ? 2 + below(5) : 0;
    scene.nodes[node].childCount = (uint16_t)childCount;
    if (!childCount)
        return;
    const size_t firstChild = scene.nodes.size();
    scene.nodes.resize(firstChild + childCount);
    scene.modelLists.resize(firstChild + childCount);
    // An offset in bytes from the node itself, as in a loaded map. (The node
    // array is given its full size before any walk, so it does not move then.)
    scene.nodes[node].childrenOffset = (int)((firstChild - node) * sizeof(GfxAabbTree));
    uint32_t given = 0;
    for (uint32_t child = 0; child < childCount; ++child)
    {
        const uint32_t share = child + 1 == childCount ? count - given : below(count - given + 1);
        std::vector<uint16_t> childModels;
        for (uint16_t model : models)
            if (below(childCount) == 0 || below(4) == 0)  // some in several children, some in none
                childModels.push_back(model);
        build(scene, firstChild + child, first + given, share, childModels, depth - 1, kind, honest);
        given += share;
    }
}

static Scene makeScene(Numbers kind, bool honest, uint32_t modelCount, uint32_t surfaceCount, uint32_t depth)
{
    Scene scene;
    scene.models.resize(modelCount);
    for (GfxStaticModelInst &model : scene.models)
        randomBox(kind, model.mins, model.maxs);
    scene.surfaces.resize(surfaceCount);
    for (GfxSurface &surface : scene.surfaces)
        randomBox(kind, surface.bounds[0], surface.bounds[1]);
    // The tree's places name the surfaces in another order than the surfaces' own.
    scene.sorted.resize(surfaceCount);
    for (uint32_t i = 0; i < surfaceCount; ++i)
        scene.sorted[i] = (uint16_t)i;
    for (uint32_t i = surfaceCount; i > 1; --i)
        std::swap(scene.sorted[i - 1], scene.sorted[below(i)]);
    std::vector<uint16_t> allModels(modelCount);
    for (uint32_t i = 0; i < modelCount; ++i)
        allModels[i] = (uint16_t)i;
    scene.nodes.reserve(1u << 16);
    scene.nodes.resize(1);
    scene.modelLists.resize(1);
    build(scene, 0, 0, surfaceCount, allModels, depth, kind, honest);
    check(scene.nodes.size() < (1u << 16), "the made-up tree outgrew its reserve");
    for (size_t node = 0; node < scene.nodes.size(); ++node)
    {
        scene.nodes[node].smodelIndexCount = (uint16_t)scene.modelLists[node].size();
        scene.nodes[node].smodelIndexes = scene.modelLists[node].data();
    }
    scene.modelVis.resize(modelCount + 1);
    scene.surfaceVis.resize(surfaceCount + 1);
    return scene;
}

static DpvsPlane randomPlane(Numbers kind)
{
    DpvsPlane plane = {};
    switch (kind == ANY ? below(2) : below(4))
    {
    case 0:  // anything of the kind
        for (float &coefficient : plane.coeffs)
            coefficient = number(kind);
        break;
    case 1:  // along one axis, as a box's own sides are
    {
        const uint32_t axis = below(3);
        plane.coeffs[axis] = below(2) ? 1.0f : -1.0f;
        plane.coeffs[3] = number(kind);
        break;
    }
    default:  // a unit normal, as a view's planes are
    {
        float lengthSq = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            plane.coeffs[axis] = (float)((int)below(2001) - 1000);
            lengthSq += plane.coeffs[axis] * plane.coeffs[axis];
        }
        const float length = lengthSq > 0.0f ? sqrtf(lengthSq) : 1.0f;
        for (int axis = 0; axis < 3; ++axis)
            plane.coeffs[axis] /= length;
        const float distance = number(kind);
        plane.coeffs[3] = distance * (below(2) ? 1.0f : 0.25f);
        break;
    }
    }
    R_SetDpvsPlaneSides(&plane);
    return plane;
}

// A plane through a corner of one of the scene's boxes, give or take a few
// steps of the last bit: the sum for that corner is zero or next to it, and
// which side of zero it lands on is a matter of the order the sum is made in.
static DpvsPlane planeThroughCorner(const Scene &scene, Numbers kind)
{
    DpvsPlane plane = randomPlane(kind);
    const float *mins, *maxs;
    switch (below(3))
    {
    case 0:
    {
        const GfxStaticModelInst &model = scene.models[below((uint32_t)scene.models.size())];
        mins = model.mins;
        maxs = model.maxs;
        break;
    }
    case 1:
    {
        const GfxSurface &surface = scene.surfaces[below((uint32_t)scene.surfaces.size())];
        mins = surface.bounds[0];
        maxs = surface.bounds[1];
        break;
    }
    default:
    {
        const GfxAabbTree &tree = scene.nodes[below((uint32_t)scene.nodes.size())];
        mins = tree.mins;
        maxs = tree.maxs;
        break;
    }
    }
    double dot = 0.0;
    for (int axis = 0; axis < 3; ++axis)
        dot += (double)(below(2) ? mins[axis] : maxs[axis]) * (double)plane.coeffs[axis];
    float d = (float)-dot;
    if (std::isfinite(d))
    {
        uint32_t bits;
        memcpy(&bits, &d, sizeof(bits));
        bits += below(5) - 2;  // up to two steps either way
        d = fromBits(bits);
    }
    plane.coeffs[3] = d;
    return plane;
}

struct Marks
{
    std::vector<byte> models, surfaces;
    bool operator==(const Marks &other) const { return models == other.models && surfaces == other.surfaces; }
};

static Marks runScalar(Scene &scene, const DpvsPlane *planes, uint32_t planeCount, const Marks &before,
                       bool drawModels, bool drawWorld, bool decals)
{
    scene.modelVis = before.models;
    scene.surfaceVis = before.surfaces;
    decltype(rgp)::World world;
    world.dpvs.sortedSurfIndex = scene.sorted.data();
    world.dpvs.smodelInsts = scene.models.data();
    world.dpvs.surfaces = scene.surfaces.data();
    rgp.world = &world;
    g_smodelVisData = scene.modelVis.data();
    g_surfaceVisData = scene.surfaceVis.data();
    rg.drawSModels = drawModels;
    rg.drawWorld = drawWorld;
    s_drawDecals.current.enabled = decals;
    // As R_AddCellStaticSurfacesInFrustum hands them over.
    DpvsClipPlaneSet clipSet;
    for (uint32_t i = 0; i < planeCount; ++i)
        clipSet.planes[i] = &planes[i];
    clipSet.count = planeCount;
    R_AddAabbTreeSurfacesInFrustum_r(scene.nodes.data(), &clipSet);
    rgp.world = nullptr;
    return Marks{scene.modelVis, scene.surfaceVis};
}

static bool s_fetchAhead; // dpvs_vec::World::fetchAhead of the next vector walk

static dpvs_vec::World vecWorld(Scene &scene, bool drawModels, bool drawWorld, bool decals)
{
    dpvs_vec::World world;
    world.modelBounds = scene.models.data()->mins;
    world.surfaceBounds = scene.surfaces.data()->bounds[0];
    world.modelStride = sizeof(GfxStaticModelInst);
    world.surfaceStride = sizeof(GfxSurface);
    world.sortedSurfIndex = scene.sorted.data();
    world.modelVis = drawModels ? scene.modelVis.data() : nullptr;
    world.surfaceVis = drawWorld ? scene.surfaceVis.data() : nullptr;
    world.decals = decals;
    world.fetchAhead = s_fetchAhead;
    return world;
}

static bool runVec(Scene &scene, const DpvsPlane *planes, uint32_t planeCount, const Marks &before,
                   bool drawModels, bool drawWorld, bool decals, Marks *after)
{
    scene.modelVis = before.models;
    scene.surfaceVis = before.surfaces;
    const bool took = dpvs_vec::AddTree(reinterpret_cast<const dpvs_vec::Tree *>(scene.nodes.data()),
                                        reinterpret_cast<const dpvs_vec::Plane *>(planes), planeCount,
                                        vecWorld(scene, drawModels, drawWorld, decals));
    *after = Marks{scene.modelVis, scene.surfaceVis};
    return took;
}

static size_t countMarked(const std::vector<byte> &marks)
{
    size_t marked = 0;
    for (byte mark : marks)
        marked += mark != 0;
    return marked;
}

struct Totals
{
    size_t walks = 0, modelsMarked = 0, surfacesMarked = 0, nothingMarked = 0, allMarked = 0;
};

static void compare(Scene &scene, Numbers kind, Totals &totals, uint32_t rounds)
{
    const size_t modelCount = scene.models.size(), surfaceCount = scene.surfaces.size();
    for (uint32_t round = 0; round < rounds; ++round)
    {
        // Every count of planes comes up often, and so does every arrangement
        // of the planes that are left deeper in the tree.
        const uint32_t planeCount = 1 + round % 16;
        DpvsPlane planes[16];
        for (uint32_t i = 0; i < planeCount; ++i)
            planes[i] = below(3) ? randomPlane(kind) : planeThroughCorner(scene, kind);
        Marks before;
        before.models.assign(modelCount + 1, 0);
        before.surfaces.assign(surfaceCount + 1, 0);
        if (below(3) == 0)
        {
            for (size_t i = 0; i < modelCount; ++i)
                before.models[i] = below(4) == 0;
            for (size_t i = 0; i < surfaceCount; ++i)
                before.surfaces[i] = below(4) == 0;
        }
        const bool drawModels = below(8) != 0, drawWorld = below(8) != 0, decals = below(2) != 0;
        // Every other walk also counts for r_dpvs_stats, which must not change a mark.
        s_dpvsCounting = (totals.walks & 1) != 0;
        const Marks scalar = runScalar(scene, planes, planeCount, before, drawModels, drawWorld, decals);
        s_dpvsCounting = false;
        Marks vec;
        // with and without asking for boxes ahead, each with and without counting
        s_fetchAhead = (totals.walks & 2) != 0;
        check(runVec(scene, planes, planeCount, before, drawModels, drawWorld, decals, &vec),
              "the vector walk refused planes the engine makes");
        if (!(scalar == vec))
        {
            size_t differing = 0;
            for (size_t i = 0; i < modelCount; ++i)
                differing += scalar.models[i] != vec.models[i];
            for (size_t i = 0; i < surfaceCount; ++i)
                differing += scalar.surfaces[i] != vec.surfaces[i];
            throw std::runtime_error("the vector walk marks other models or surfaces than the scalar walk: " +
                                     std::to_string(differing) + " differ (kind " + std::to_string(kind) + ", " +
                                     std::to_string(planeCount) + " planes, round " + std::to_string(round) + ")");
        }
        check(vec.models[modelCount] == 0 && vec.surfaces[surfaceCount] == 0, "a mark past the end");
        const size_t models = countMarked(vec.models) - countMarked(before.models);
        const size_t surfaces = countMarked(vec.surfaces) - countMarked(before.surfaces);
        ++totals.walks;
        totals.modelsMarked += models;
        totals.surfacesMarked += surfaces;
        totals.nothingMarked += !models && !surfaces;
        totals.allMarked += countMarked(vec.models) == modelCount && countMarked(vec.surfaces) == surfaceCount;
    }
}

// A plane whose side[] the engine does not make: the scalar code would read
// another float of the box than the vector code can, so the vector code has
// to say no and leave every mark alone.
static void checkRefusals()
{
    Scene scene = makeScene(WHOLE, true, 40, 60, 3);
    Marks before;
    before.models.assign(scene.models.size() + 1, 0);
    before.surfaces.assign(scene.surfaces.size() + 1, 0);
    before.models[3] = 1;
    DpvsPlane planes[17];
    for (DpvsPlane &plane : planes)
        plane = randomPlane(WHOLE);
    Marks after;
    check(runVec(scene, planes, 16, before, true, true, true, &after), "sixteen good planes refused");
    // Each axis on its own: every offset but its two, the others as they were.
    const uint8_t good[3][2] = {{0, 12}, {4, 16}, {8, 20}};
    size_t refused = 0;
    for (int axis = 0; axis < 3; ++axis)
    {
        for (uint32_t side = 0; side < 256; ++side)
        {
            if (side == good[axis][0] || side == good[axis][1])
                continue;
            const uint32_t at = (side + (uint32_t)axis) % 16;
            DpvsPlane changed[16];
            memcpy(changed, planes, sizeof(changed));
            changed[at].side[axis] = (uint8_t)side;
            check(!runVec(scene, changed, 16, before, true, true, true, &after), "a plane with other sides was taken");
            check(after == before, "a refused walk marked something");
            ++refused;
        }
    }
    check(refused == 3 * 254, "not every other side was tried");
    check(!runVec(scene, planes, 0, before, true, true, true, &after) && after == before, "no planes were taken");
    check(!runVec(scene, planes, 17, before, true, true, true, &after) && after == before,
          "seventeen planes were taken");
}

// The debug boxes are the scalar walk's alone; the engine keeps to it while
// r_showAabbTrees is on. Here they have to stay off, or the reference would
// be counting boxes that this check does not look at.
static void checkNoDebugBoxes()
{
    check(s_frontEndData.debugGlobals.boxes == 0, "the scalar walk drew debug boxes");
}

static double seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void bench()
{
    s_state = 0x9e3779b9u;
    Scene scene = makeScene(COARSE, true, 600, 3000, 6);
    std::vector<std::vector<DpvsPlane>> sets(64);
    for (std::vector<DpvsPlane> &set : sets)
    {
        set.resize(4 + below(6));
        for (DpvsPlane &plane : set)
        {
            plane = randomPlane(COARSE);
            // Keep most of the scene in front, as a view's planes do.
            plane.coeffs[3] = fabsf(plane.coeffs[3]) + 2000.0f;
        }
    }
    Marks clear;
    clear.models.assign(scene.models.size() + 1, 0);
    clear.surfaces.assign(scene.surfaces.size() + 1, 0);
    const int repeats = 200;
    size_t marked[2] = {};
    double took[2] = {};
    for (int which = 0; which < 2; ++which)
    {
        const double start = seconds();
        for (int repeat = 0; repeat < repeats; ++repeat)
        {
            for (const std::vector<DpvsPlane> &set : sets)
            {
                Marks after;
                if (which)
                    runVec(scene, set.data(), (uint32_t)set.size(), clear, true, true, true, &after);
                else
                    after = runScalar(scene, set.data(), (uint32_t)set.size(), clear, true, true, true);
                marked[which] += countMarked(after.surfaces) + countMarked(after.models);
            }
        }
        took[which] = seconds() - start;
    }
    check(marked[0] == marked[1], "the bench's two walks marked different counts");
    // The copies of the marks are in both; what differs is the walk.
    const double walks = (double)repeats * sets.size();
    printf("bench: %zu tree nodes, %zu marked a walk on average\n", scene.nodes.size(),
           marked[0] / (size_t)walks);
    printf("bench: scalar %.2f us a walk, four planes at a time %.2f us a walk\n", took[0] / walks * 1e6,
           took[1] / walks * 1e6);
}

int main(int argc, char **argv)
{
    try
    {
        Totals totals;
        for (int kind = 0; kind < NUMBERS; ++kind)
        {
            for (int honest = 0; honest < 2; ++honest)
            {
                for (uint32_t scenes = 0; scenes < 12; ++scenes)
                {
                    // One at a time: a call's arguments are worked out in an order of the compiler's choosing.
                    const uint32_t modelCount = 1 + below(120);
                    const uint32_t surfaceCount = 1 + below(400);
                    const uint32_t depth = 1 + below(5);
                    Scene scene = makeScene((Numbers)kind, honest != 0, modelCount, surfaceCount, depth);
                    compare(scene, (Numbers)kind, totals, 160);
                }
            }
        }
        checkRefusals();
        checkNoDebugBoxes();
        // A check that only ever saw "all in" or "all out" would prove little.
        check(totals.nothingMarked * 10 > totals.walks && totals.allMarked * 50 > totals.walks &&
                  (totals.walks - totals.nothingMarked - totals.allMarked) * 4 > totals.walks,
              "the made-up walks are not a mix of all in, all out and part in");
        check(s_dpvsCounted != 0, "no scalar walk counted for r_dpvs_stats");
        // The scenes come from an integer generator and plain IEEE single
        // sums, so every architecture walks the same ones and marks as many.
        // Change these only together with the scenes above.
        check(totals.walks == 11520 && totals.modelsMarked == 175286 && totals.surfacesMarked == 387900,
              "the walks agree, but not on what every architecture marks: " + std::to_string(totals.walks) +
                  " walks, " + std::to_string(totals.modelsMarked) + " models, " +
                  std::to_string(totals.surfacesMarked) + " surfaces");
        printf("static culling: %zu walks agree (%zu models and %zu surfaces marked; %zu walks marked nothing, "
               "%zu everything)\n",
               totals.walks, totals.modelsMarked, totals.surfacesMarked, totals.nothingMarked, totals.allMarked);
        if (argc > 1 && !strcmp(argv[1], "--bench"))
            bench();
    }
    catch (const std::exception &error)
    {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
"""


def build_harness(root: Path) -> str:
    gfx = (root / "src/gfx_d3d/r_gfx.h").read_text()
    dpvs_h = (root / "src/gfx_d3d/r_dpvs.h").read_text()
    dpvs_cpp = (root / "src/gfx_d3d/r_dpvs.cpp").read_text()
    static_cpp = (root / "src/gfx_d3d/r_dpvs_static.cpp").read_text()
    stats_h = (root / "src/gfx_d3d/r_dpvs_stats.h").read_text()
    parts = [HARNESS_TOP]
    for head in (
        "struct srfTriangles_t",
        "struct GfxStaticModelInst",
        "struct GfxSurface",
        "struct GfxAabbTree",
        "struct DpvsPlane",
    ):
        parts.append(declaration(gfx, head))
    parts.append(declaration(dpvs_h, "struct DpvsClipPlaneSet"))
    parts.append(declaration(stats_h, "enum DpvsCount"))
    parts.append(HARNESS_WORLD)
    # The production function that sets a plane's side[] from its coefficients,
    # and the production scalar walk.
    parts.append(block(dpvs_cpp, dpvs_cpp.index("void R_SetDpvsPlaneSides(DpvsPlane *plane)")) + "\n")
    parts.append(
        block(static_cpp, static_cpp.index("void R_AddAabbTreeSurfacesInFrustum_r(const GfxAabbTree *tree"))
        + "\n"
    )
    parts.append(HARNESS_BOTTOM)
    return "".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="host C++ compiler")
    parser.add_argument("--bench", action="store_true", help="also time the two walks")
    parser.add_argument("--sanitize", action="store_true", help="build with the address and undefined sanitizers")
    parser.add_argument("--keep", metavar="DIR", help="write the harness source here and keep it")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    harness = build_harness(root)
    with tempfile.TemporaryDirectory(prefix="static-cull-vec-") as temp:
        work = Path(args.keep) if args.keep else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        source = work / "static_cull_vec_harness.cpp"
        source.write_text(harness)
        binary = work / "static_cull_vec_harness"
        # The engine's float rule: no fused multiply-add, on any host.
        command = [
            *shlex.split(args.cxx),
            "-std=c++20",
            "-O2",
            "-g",
            "-ffp-contract=off",
            "-Wall",
            "-Wextra",
            "-Wno-unused-variable",
            "-Wno-unused-but-set-variable",
            "-Wno-unused-function",
            "-Wno-psabi",
            f"-I{root / 'src/gfx_d3d'}",
            str(source),
            str(root / "src/gfx_d3d/r_dpvs_static_vec.cpp"),
            "-o",
            str(binary),
        ]
        if args.sanitize:
            command[3:3] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
        built = subprocess.run(command, capture_output=True, text=True)
        if built.returncode:
            print(built.stdout + built.stderr)
            print("FAIL: the harness did not compile")
            return 1
        if built.stderr.strip():
            print(built.stderr)
        ran = subprocess.run([str(binary)] + (["--bench"] if args.bench else []), capture_output=True, text=True)
        print((ran.stdout + ran.stderr).strip())
        if ran.returncode:
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
