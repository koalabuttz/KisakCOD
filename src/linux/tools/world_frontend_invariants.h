#pragma once

// Map-independent checks for one renderer front-end frame over a world, and the
// spawn-point camera list the world front-end probe derives its views from.
//
// This header deliberately pulls in no engine header: the probe
// (world_frontend_probe.cpp) reads the engine's world and frame state into the
// plain records below, and tests/linux_asset_export/world_frontend_invariants_test.cpp
// drives the same checks with hand-built records, so every rule here is exercised
// without a zone, a renderer or game data.
//
// What a frame must satisfy follows from the production code, not from any one
// map:
//
//   * R_CellForPoint put the camera in a cell, and the portal walk
//     (R_AddWorldSurfacesDpvs) marked that cell visible.
//   * A surface is marked visible only through a visible cell: an AABB tree node
//     or a cull group of that cell lists it (r_dpvs_static.cpp indexes both
//     through dpvs.sortedSurfIndex).
//   * R_AddAllBspDrawSurfacesCamera and R_AddAllBspDrawSurfacesCameraNonlit walk
//     one surface range per stage (dpvs.litSurfs*, decalSurfs*, emissiveSurfs*)
//     and add every visible surface of that range exactly once, grouping runs of
//     equal dpvs.surfaceMaterials keys into one draw surface. So each emitted
//     batch holds visible surfaces of its own stage's range whose key matches the
//     batch key outside the objectId bits (objectId is the batch's offset into
//     the primitive stream), no surface is emitted twice, and none of a stage's
//     visible surfaces is left out.
//
// Two further properties compare frames and are checked by the probe with the
// helpers at the end: the walk from a camera cell sees a subset of what the
// frustum alone sees from the same eye (portals only add planes), and the same
// view run twice gives the same frame.
//
// Not a rule here: that emitted keys ascend. A BSP stage is emitted in surface
// order (R_MergeAndEmitDrawSurfLists does not sort a single list), so key order
// is a property of how the map compiler ordered the surfaces. The probe reports
// it as an observation (CountKeyDescents) instead of failing on it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace kisak_world_frontend
{
enum Stage : unsigned
{
    kStageLit = 0,
    kStageDecal = 1,
    kStageEmissive = 2,
    kStageCount = 3,
};

inline const char *StageName(unsigned stage)
{
    switch (stage)
    {
    case kStageLit:
        return "lit";
    case kStageDecal:
        return "decal";
    case kStageEmissive:
        return "emissive";
    default:
        return "unknown";
    }
}

// GfxDrawSurfFields::objectId occupies the low 16 bits of the packed key.
constexpr uint64_t kObjectIdMask = 0xFFFFull;

inline uint64_t KeyWithoutObjectId(uint64_t key)
{
    return key & ~kObjectIdMask;
}

struct SurfaceRange
{
    uint32_t begin = 0; // first surface index
    uint32_t end = 0;   // one past the last
};

// What the checks need to know about the world, read once after the load.
struct WorldShape
{
    uint32_t surfaceCount = 0;
    // Per cell: the sorted, unique surface indices its AABB trees and cull groups list.
    std::vector<std::vector<uint32_t>> cellSurfaces;
    SurfaceRange stageRange[kStageCount];
    // Per surface: its dpvs.surfaceMaterials key.
    std::vector<uint64_t> surfaceKey;
};

// One draw surface the front end emitted for a BSP stage.
struct Batch
{
    uint64_t key = 0;
    std::vector<uint32_t> surfaces;
};

struct Frame
{
    int cameraCell = -1;
    std::vector<int> visibleCells;         // sorted, unique
    std::vector<uint32_t> visibleSurfaces; // sorted, unique
    std::vector<Batch> batches[kStageCount];
};

inline bool Contains(const std::vector<uint32_t> &sorted, uint32_t value)
{
    return std::binary_search(sorted.begin(), sorted.end(), value);
}

inline bool Contains(const std::vector<int> &sorted, int value)
{
    return std::binary_search(sorted.begin(), sorted.end(), value);
}

inline bool IsSortedUnique(const std::vector<uint32_t> &values)
{
    return std::adjacent_find(values.begin(), values.end(),
                              [](uint32_t a, uint32_t b) { return a >= b; }) == values.end();
}

inline bool IsSortedUnique(const std::vector<int> &values)
{
    return std::adjacent_find(values.begin(), values.end(), [](int a, int b) { return a >= b; }) == values.end();
}

// A world that cannot be checked is reported as such, before any frame.
inline std::vector<std::string> CheckWorldShape(const WorldShape &world)
{
    std::vector<std::string> problems;
    if (world.surfaceKey.size() != world.surfaceCount)
        problems.push_back("surface key table has " + std::to_string(world.surfaceKey.size()) + " entries for " +
                           std::to_string(world.surfaceCount) + " surfaces");
    for (size_t cell = 0; cell < world.cellSurfaces.size(); ++cell)
    {
        const std::vector<uint32_t> &surfaces = world.cellSurfaces[cell];
        if (!IsSortedUnique(surfaces))
            problems.push_back("cell " + std::to_string(cell) + " surface list is not sorted and unique");
        else if (!surfaces.empty() && surfaces.back() >= world.surfaceCount)
            problems.push_back("cell " + std::to_string(cell) + " lists surface " + std::to_string(surfaces.back()) +
                               " of " + std::to_string(world.surfaceCount));
    }
    for (unsigned stage = 0; stage < kStageCount; ++stage)
    {
        const SurfaceRange &range = world.stageRange[stage];
        if (range.begin > range.end || range.end > world.surfaceCount)
            problems.push_back(std::string(StageName(stage)) + " surface range " + std::to_string(range.begin) +
                               ".." + std::to_string(range.end) + " does not fit " +
                               std::to_string(world.surfaceCount) + " surfaces");
    }
    return problems;
}

// Every rule in the header comment that one frame can be held to. Returns one
// line per violation, at most `limit` lines; empty means the frame holds them all.
inline std::vector<std::string> CheckFrame(const WorldShape &world, const Frame &frame, size_t limit = 16)
{
    std::vector<std::string> problems;
    auto report = [&](const std::string &line) {
        if (problems.size() < limit)
            problems.push_back(line);
    };
    const int cellCount = int(world.cellSurfaces.size());

    if (frame.cameraCell < 0 || frame.cameraCell >= cellCount)
        report("camera cell " + std::to_string(frame.cameraCell) + " is not one of " + std::to_string(cellCount) +
               " cells");
    else if (!Contains(frame.visibleCells, frame.cameraCell))
        report("camera cell " + std::to_string(frame.cameraCell) + " is not marked visible");

    if (!IsSortedUnique(frame.visibleCells))
        report("visible cell list is not sorted and unique");
    else if (!frame.visibleCells.empty() && (frame.visibleCells.front() < 0 || frame.visibleCells.back() >= cellCount))
        report("visible cell outside 0.." + std::to_string(cellCount));

    if (!IsSortedUnique(frame.visibleSurfaces))
        report("visible surface list is not sorted and unique");
    for (uint32_t surface : frame.visibleSurfaces)
    {
        if (surface >= world.surfaceCount)
        {
            report("visible surface " + std::to_string(surface) + " is not one of " +
                   std::to_string(world.surfaceCount) + " surfaces");
            continue;
        }
        bool listed = false;
        for (int cell : frame.visibleCells)
        {
            if (cell >= 0 && cell < cellCount && Contains(world.cellSurfaces[size_t(cell)], surface))
            {
                listed = true;
                break;
            }
        }
        if (!listed)
            report("visible surface " + std::to_string(surface) + " is listed by no visible cell");
    }

    std::vector<uint32_t> emitted;
    for (unsigned stage = 0; stage < kStageCount; ++stage)
    {
        const SurfaceRange &range = world.stageRange[stage];
        std::vector<uint32_t> stageEmitted;
        for (size_t b = 0; b < frame.batches[stage].size(); ++b)
        {
            const Batch &batch = frame.batches[stage][b];
            const std::string where = std::string(StageName(stage)) + " batch " + std::to_string(b);
            if (batch.surfaces.empty())
                report(where + " is empty");
            for (uint32_t surface : batch.surfaces)
            {
                if (surface < range.begin || surface >= range.end)
                {
                    report(where + " holds surface " + std::to_string(surface) + " outside the stage range " +
                           std::to_string(range.begin) + ".." + std::to_string(range.end));
                    continue;
                }
                if (!Contains(frame.visibleSurfaces, surface))
                    report(where + " holds surface " + std::to_string(surface) + ", which is not visible");
                if (surface < world.surfaceKey.size() &&
                    KeyWithoutObjectId(world.surfaceKey[surface]) != KeyWithoutObjectId(batch.key))
                    report(where + " key does not match surface " + std::to_string(surface) + "'s material key");
                stageEmitted.push_back(surface);
            }
        }
        std::sort(stageEmitted.begin(), stageEmitted.end());
        // Every visible surface of the stage's range is emitted by that stage.
        auto first = std::lower_bound(frame.visibleSurfaces.begin(), frame.visibleSurfaces.end(), range.begin);
        auto last = std::lower_bound(frame.visibleSurfaces.begin(), frame.visibleSurfaces.end(), range.end);
        for (auto it = first; it != last; ++it)
            if (!std::binary_search(stageEmitted.begin(), stageEmitted.end(), *it))
                report(std::string(StageName(stage)) + " stage leaves visible surface " + std::to_string(*it) +
                       " out");
        emitted.insert(emitted.end(), stageEmitted.begin(), stageEmitted.end());
    }
    std::sort(emitted.begin(), emitted.end());
    auto dup = std::adjacent_find(emitted.begin(), emitted.end());
    if (dup != emitted.end())
        report("surface " + std::to_string(*dup) + " is emitted more than once");
    return problems;
}

// Visible surfaces that no stage range covers. They are legal (a surface can
// have no camera technique), so the probe counts them instead of failing.
inline size_t CountVisibleOutsideStages(const WorldShape &world, const Frame &frame)
{
    size_t count = 0;
    for (uint32_t surface : frame.visibleSurfaces)
    {
        bool inStage = false;
        for (const SurfaceRange &range : world.stageRange)
            inStage = inStage || (surface >= range.begin && surface < range.end);
        count += !inStage;
    }
    return count;
}

// How often a stage's emitted key goes down from one batch to the next.
inline size_t CountKeyDescents(const std::vector<Batch> &batches)
{
    size_t descents = 0;
    for (size_t i = 1; i < batches.size(); ++i)
        descents += KeyWithoutObjectId(batches[i].key) < KeyWithoutObjectId(batches[i - 1].key);
    return descents;
}

inline bool IsSubset(const std::vector<uint32_t> &sortedSmall, const std::vector<uint32_t> &sortedLarge)
{
    return std::includes(sortedLarge.begin(), sortedLarge.end(), sortedSmall.begin(), sortedSmall.end());
}

inline bool SameFrame(const Frame &a, const Frame &b)
{
    if (a.cameraCell != b.cameraCell || a.visibleCells != b.visibleCells || a.visibleSurfaces != b.visibleSurfaces)
        return false;
    for (unsigned stage = 0; stage < kStageCount; ++stage)
    {
        if (a.batches[stage].size() != b.batches[stage].size())
            return false;
        for (size_t i = 0; i < a.batches[stage].size(); ++i)
            if (KeyWithoutObjectId(a.batches[stage][i].key) != KeyWithoutObjectId(b.batches[stage][i].key) ||
                a.batches[stage][i].surfaces != b.batches[stage][i].surfaces)
                return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Camera views from the map's own entity string.
//
// Player spawn points are places the map author made reachable, so the eye
// above one is in open space and inside some cell. The rule names no map:
// every entity whose classname starts with "mp_" and contains "_spawn" is a
// player spawn, and its eye is kSpawnEyeHeight above the origin;
// mp_global_intermission is a camera the map author placed, so its origin is
// the eye. Entities are taken in file order, up to the caller's limit.
// ---------------------------------------------------------------------------

// The standing view height in the original bg_* player code.
constexpr float kSpawnEyeHeight = 60.0f;

struct SpawnView
{
    std::string classname;
    float eye[3] = {0, 0, 0};
    float angles[3] = {0, 0, 0};
};

struct SpawnScan
{
    std::vector<SpawnView> views;
    size_t entities = 0;       // entity blocks parsed
    size_t matching = 0;       // entities that matched the rule (views holds at most the limit)
    std::string error;         // set when the entity string is malformed
};

inline bool IsPlayerSpawnClass(const std::string &classname)
{
    return classname.rfind("mp_", 0) == 0 && classname.find("_spawn") != std::string::npos;
}

inline bool ParseVector(const std::string &text, float out[3])
{
    const char *p = text.c_str();
    for (int i = 0; i < 3; ++i)
    {
        char *end = nullptr;
        const double value = std::strtod(p, &end);
        if (end == p || !std::isfinite(value))
            return false;
        out[i] = float(value);
        p = end;
    }
    while (*p == ' ' || *p == '\t')
        ++p;
    return *p == '\0';
}

// The entity string is a sequence of { "key" "value" ... } blocks. A quoted
// token cannot contain a quote; anything else outside a token is whitespace.
inline SpawnScan ScanSpawnViews(const char *text, size_t length, size_t limit)
{
    SpawnScan scan;
    size_t pos = 0;
    auto skipSpace = [&]() {
        while (pos < length && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
            ++pos;
    };
    auto readQuoted = [&](std::string &out) -> bool {
        if (pos >= length || text[pos] != '"')
            return false;
        const size_t start = ++pos;
        while (pos < length && text[pos] != '"')
            ++pos;
        if (pos >= length)
            return false;
        out.assign(text + start, pos - start);
        ++pos;
        return true;
    };
    for (;;)
    {
        skipSpace();
        if (pos >= length || text[pos] == '\0')
            break;
        if (text[pos] != '{')
        {
            scan.error = "expected '{' at byte " + std::to_string(pos);
            return scan;
        }
        ++pos;
        ++scan.entities;
        std::string classname, origin, angles;
        bool hasOrigin = false;
        for (;;)
        {
            skipSpace();
            if (pos < length && text[pos] == '}')
            {
                ++pos;
                break;
            }
            std::string key, value;
            if (!readQuoted(key))
            {
                scan.error = "expected a quoted key at byte " + std::to_string(pos);
                return scan;
            }
            skipSpace();
            if (!readQuoted(value))
            {
                scan.error = "expected a quoted value for '" + key + "' at byte " + std::to_string(pos);
                return scan;
            }
            if (key == "classname")
                classname = value;
            else if (key == "origin")
                origin = value, hasOrigin = true;
            else if (key == "angles")
                angles = value;
        }
        const bool spawn = IsPlayerSpawnClass(classname);
        if (!spawn && classname != "mp_global_intermission")
            continue;
        SpawnView view;
        view.classname = classname;
        if (!hasOrigin || !ParseVector(origin, view.eye))
        {
            scan.error = "entity " + std::to_string(scan.entities - 1) + " (" + classname + ") has no valid origin";
            return scan;
        }
        if (!angles.empty() && !ParseVector(angles, view.angles))
        {
            scan.error = "entity " + std::to_string(scan.entities - 1) + " (" + classname + ") has invalid angles";
            return scan;
        }
        if (spawn)
            view.eye[2] += kSpawnEyeHeight;
        ++scan.matching;
        if (scan.views.size() < limit)
            scan.views.push_back(view);
    }
    return scan;
}
} // namespace kisak_world_frontend
