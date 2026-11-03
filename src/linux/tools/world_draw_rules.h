#pragma once

// What the world draw probe holds a drawn frame to, as pure functions over
// plain pixels and counts, so an asset-free test can check them
// (tests/linux_asset_export/world_draw_rules_test.cpp). No engine header, no
// D3D9 header: the probe reads the back buffer and the renderer's counters and
// hands them over.
//
// There is no reference image to compare with, and none may be stored, so the
// rules say what any drawn view of a real map has and an undrawn or broken frame
// lacks: many colours, no one colour covering most of the frame, detail spread
// over the frame rather than in one corner, a brightness that is neither black
// nor white, the same pixels when the same view is drawn again, and different
// pixels when the camera moves.
//
// Each check returns an empty string when the frame is right, otherwise one
// sentence naming what is wrong.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kisak_world_draw
{
// The frame is cut into kTiles x kTiles tiles; a tile has detail when it holds
// more than one colour.
constexpr uint32_t kTiles = 4;
constexpr uint32_t kTileCount = kTiles * kTiles;

// Thresholds. A cleared or single-material frame has one or a handful of
// colours; a textured, lit world view has thousands.
constexpr size_t kMinDistinctColors = 256;
constexpr double kMaxDominantShare = 0.5;    // of all pixels
constexpr uint32_t kMinDetailTiles = 12;     // of kTileCount
constexpr double kMinMeanLuma = 4.0;         // 0..255
constexpr double kMaxMeanLuma = 251.0;
// Counting stops here; a real view passes kMinDistinctColors long before.
constexpr size_t kDistinctColorCap = 1u << 20;

struct FrameStats
{
    uint32_t width = 0;
    uint32_t height = 0;
    size_t distinctColors = 0;     // capped at kDistinctColorCap
    double dominantShare = 0;      // the most common colour's share of the pixels
    uint32_t dominantColor = 0;    // 0xRRGGBB
    double meanLuma = 0;           // Rec. 601 luma, 0..255
    double lumaStdDev = 0;
    uint32_t detailTiles = 0;      // of kTileCount
    uint64_t digest = 0;           // FNV-1a over the RGB bytes, row by row
};

// Rec. 601 luma in thousandths, as an integer so that sums over a frame are exact.
inline int64_t LumaMilli(uint32_t rgb)
{
    return 299 * int64_t((rgb >> 16) & 0xFF) + 587 * int64_t((rgb >> 8) & 0xFF) + 114 * int64_t(rgb & 0xFF);
}

inline double Luma(uint32_t rgb)
{
    return double(LumaMilli(rgb)) / 1000.0;
}

// rows: height rows of width 32-bit pixels laid out as the D3D9 X8R8G8B8 and
// A8R8G8B8 formats store them (blue in the low byte); pitch is in bytes. The
// alpha or unused byte is ignored.
inline FrameStats MeasureFrame(const uint8_t *rows, uint32_t width, uint32_t height, size_t pitch)
{
    FrameStats stats;
    stats.width = width;
    stats.height = height;
    if (!rows || !width || !height)
        return stats;
    std::unordered_map<uint32_t, uint32_t> counts;
    counts.reserve(1u << 16);
    bool capped = false;
    std::vector<uint32_t> tileFirst(kTileCount, 0);
    std::vector<uint8_t> tileSeen(kTileCount, 0), tileDetail(kTileCount, 0);
    uint64_t digest = 0xcbf29ce484222325ull;
    // Luma sums are taken around the first pixel's luma, in integers: exact, and
    // a frame of one colour has a spread of exactly zero.
    const int64_t lumaOrigin = LumaMilli((uint32_t(rows[2]) << 16) | (uint32_t(rows[1]) << 8) | rows[0]);
    int64_t sum = 0, sumSq = 0;
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t *row = rows + size_t(y) * pitch;
        const uint32_t tileRow = y * kTiles / height;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t *p = row + size_t(x) * 4;
            const uint32_t rgb = (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
            for (int c = 2; c >= 0; --c)
                digest = (digest ^ p[c]) * 0x100000001b3ull;
            const int64_t luma = LumaMilli(rgb) - lumaOrigin;
            sum += luma;
            sumSq += luma * luma;
            auto found = counts.find(rgb);
            if (found != counts.end())
                ++found->second;
            else if (counts.size() < kDistinctColorCap)
                counts.emplace(rgb, 1u);
            else
                capped = true;
            const uint32_t tile = tileRow * kTiles + x * kTiles / width;
            if (!tileSeen[tile])
                tileSeen[tile] = 1, tileFirst[tile] = rgb;
            else if (tileFirst[tile] != rgb)
                tileDetail[tile] = 1;
        }
    }
    const double pixels = double(width) * double(height);
    stats.distinctColors = capped ? kDistinctColorCap : counts.size();
    uint32_t best = 0;
    for (const auto &[rgb, count] : counts)
        if (count > best || (count == best && rgb < stats.dominantColor))
            best = count, stats.dominantColor = rgb;
    stats.dominantShare = double(best) / pixels;
    const double meanOffset = double(sum) / pixels; // thousandths, from lumaOrigin
    stats.meanLuma = (double(lumaOrigin) + meanOffset) / 1000.0;
    const double variance = double(sumSq) / pixels - meanOffset * meanOffset;
    stats.lumaStdDev = variance > 0 ? std::sqrt(variance) / 1000.0 : 0;
    for (uint8_t detail : tileDetail)
        stats.detailTiles += detail;
    stats.digest = digest;
    return stats;
}

// One drawn view of a real map, against the window it was drawn for.
inline std::string CheckFrame(const FrameStats &frame, uint32_t width, uint32_t height)
{
    if (frame.width != width || frame.height != height)
        return "the back buffer is " + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
               ", not the " + std::to_string(width) + "x" + std::to_string(height) + " window";
    if (frame.distinctColors < kMinDistinctColors)
        return "the frame has " + std::to_string(frame.distinctColors) + " colours, fewer than the " +
               std::to_string(kMinDistinctColors) + " any textured view has (nothing or almost nothing was drawn)";
    if (frame.dominantShare > kMaxDominantShare)
        return "one colour covers " + std::to_string(int(frame.dominantShare * 100.0 + 0.5)) +
               "% of the frame (a clear colour or one flat surface fills the view)";
    if (frame.detailTiles < kMinDetailTiles)
        return "only " + std::to_string(frame.detailTiles) + " of " + std::to_string(kTileCount) +
               " tiles of the frame have any detail (the drawing covers part of the view)";
    if (frame.meanLuma < kMinMeanLuma)
        return "the frame is black (mean luma " + std::to_string(frame.meanLuma) + ")";
    if (frame.meanLuma > kMaxMeanLuma)
        return "the frame is white (mean luma " + std::to_string(frame.meanLuma) + ")";
    return {};
}

// The same view drawn again with the same scene time: a renderer that draws from
// the scene, not from leftovers, settles on the same pixels. The first draws of a
// view differ while per-frame state settles (the static model cache fills a
// bounded amount each frame), so the probe draws a view until two draws in a row
// match, up to a limit, and this rule asks that the last two match.
inline std::string CheckRepeatable(const std::vector<uint64_t> &digests)
{
    if (digests.size() < 2)
        return "the view was drawn fewer than two times";
    if (digests[digests.size() - 1] != digests[digests.size() - 2])
        return "the view never settled: the last two of " + std::to_string(digests.size()) +
               " draws gave different pixels";
    return {};
}

// A view has settled when two draws in a row give the same pixels and the second
// moved nothing new into the static model cache: the cache is the per-frame state
// known to change a view's surfaces from draw to draw, and pixels can match by
// chance while it is still filling.
inline std::string CheckSettled(const std::vector<uint64_t> &digests, uint32_t lastCachePatches)
{
    std::string problem = CheckRepeatable(digests);
    if (!problem.empty())
        return problem;
    if (lastCachePatches != 0)
        return "the static model cache was still filling on the last draw (" + std::to_string(lastCachePatches) +
               " surfaces)";
    return {};
}

// Two different cameras over the same map: the image must follow the camera.
inline std::string CheckViewsDiffer(uint64_t first, uint64_t second)
{
    if (first == second)
        return "two different camera positions gave the same pixels (the view does not follow the camera)";
    return {};
}

// What the renderer itself counted for the frame: the front end emitted world
// surfaces for the camera, and the backend issued draw calls for the world.
struct DrawCounts
{
    uint32_t litSurfaces = 0;       // GfxViewInfo::litInfo.drawSurfCount
    uint32_t decalSurfaces = 0;     // decalInfo
    uint32_t emissiveSurfaces = 0;  // emissiveInfo
    uint32_t worldDrawCalls = 0;    // viewStats[0].primStats[GFX_PRIM_STATS_WORLD].primCount (the scene, not the shadow maps)
    uint32_t worldTriangles = 0;    // .triCount
};

inline std::string CheckDrawCounts(const DrawCounts &counts)
{
    if (counts.litSurfaces + counts.decalSurfaces + counts.emissiveSurfaces == 0)
        return "the front end emitted no draw surface for the camera";
    if (counts.worldDrawCalls == 0)
        return "the backend issued no world draw call";
    if (counts.worldTriangles == 0)
        return "the backend's world draw calls drew no triangle";
    return {};
}

// The static models and the sun's shadow map in the same frame, as the renderer
// counted them. The front end's counts are read after R_RenderScene, the
// backend's after the frame was issued.
struct SceneCounts
{
    uint32_t staticModels = 0;          // the world's static models (dpvs.smodelCount)
    uint32_t staticModelsDrawn = 0;     // the ones the camera's front end kept (smodelVisData[0] still set)
    uint32_t staticModelSurfaces = 0;   // their camera draw surfaces: lit, decal and emissive
    uint32_t staticModelDrawCalls = 0;  // primStats[SMODELCACHED] + [SMODELRIGID] primCount, both stats views
    uint32_t staticModelTriangles = 0;  // .triCount
    bool sunLight = false;              // the world has a sun (sunPrimaryLightIndex)
    bool sunUsed = false;               // the camera's surfaces are lit by it (scene.shadowableLightIsUsed)
    bool shadowMaps = false;            // shadow maps are on (sm_enable) and the scene is drawn the standard way
    bool sunShadowDrawn = false;        // the front end set the sun's shadow map up (rg.drawSunShadow)
    uint32_t sunShadowSurfaces = 0;     // its draw surfaces, both partitions
    uint32_t sunShadowStaticModelSurfaces = 0; // of those, the static models'
};

// What every frame's counts must agree on: static models the front end kept have
// surfaces, the backend drew them, and the sun's shadow map is drawn exactly when
// the sun lights the view with shadow maps on, and then has something in it.
inline std::string CheckSceneCounts(const SceneCounts &counts)
{
    if (counts.staticModelsDrawn > counts.staticModels)
        return "the camera kept " + std::to_string(counts.staticModelsDrawn) + " static models, but the world has " +
               std::to_string(counts.staticModels);
    if (counts.staticModelsDrawn > 0 && counts.staticModelSurfaces == 0)
        return "the camera kept " + std::to_string(counts.staticModelsDrawn) +
               " static models, but the front end emitted no surface for them";
    if (counts.staticModelSurfaces > 0 && counts.staticModelDrawCalls == 0)
        return "the front end emitted " + std::to_string(counts.staticModelSurfaces) +
               " static model surfaces, but the backend issued no static model draw call";
    if (counts.staticModelDrawCalls > 0 && counts.staticModelTriangles == 0)
        return "the backend's static model draw calls drew no triangle";
    if (counts.sunUsed && !counts.sunLight)
        return "the camera's surfaces use a sun the world does not have";
    const bool wanted = counts.sunUsed && counts.shadowMaps;
    if (wanted && !counts.sunShadowDrawn)
        return "the sun lights the view with shadow maps on, but its shadow map was not drawn";
    if (!wanted && counts.sunShadowDrawn)
        return "the sun's shadow map was drawn for a view it does not light, or with shadow maps off";
    if (counts.sunShadowDrawn && counts.sunShadowSurfaces == 0)
        return "the sun's shadow map was drawn with nothing in it";
    if (counts.sunShadowStaticModelSurfaces > counts.sunShadowSurfaces)
        return "the sun's shadow map has more static model surfaces than surfaces";
    return {};
}

// What a map's own camera must see of a map that has them: some of its static
// models, and the sun's shadow. Applied only at the cameras the map author placed
// (intermission and spawn points), which look at the playable, sunlit map.
inline std::string CheckSceneInView(const SceneCounts &counts)
{
    if (counts.staticModels > 0 && counts.staticModelsDrawn == 0)
        return "the world has " + std::to_string(counts.staticModels) + " static models and the camera drew none";
    if (counts.sunLight && !counts.sunShadowDrawn)
        return "the world has a sun and the camera drew no sun shadow map";
    return {};
}

// The map's effects in one frame, as the free camera's effects host and the
// renderer counted them. The draw mode runs no effects system; the free camera
// runs it as the cgame does. The front end's counts are read after
// R_RenderScene, the backend's after the frame was issued.
struct EffectCounts
{
    bool running = false;      // the effects system was updated for this frame
    uint32_t effects = 0;      // effects alive in the system
    uint32_t elems = 0;        // their elements and trail elements alive
    uint32_t clouds = 0;       // particle clouds queued for the frame
    uint32_t surfaces = 0;     // draw surfaces in the six effect regions (lit, emissive; plain, auto, decal)
    uint32_t meshes = 0;       // code meshes the frame's effects generated (frontEndDataOut->codeMeshCount)
    uint32_t drawCalls = 0;    // primStats[FX] primCount, both stats views: the effect meshes, and the sun's flare
    uint32_t triangles = 0;    // .triCount
};

// What every frame's effect counts must agree on: nothing of an effect without
// the system running, a draw surface for each mesh, something alive behind every
// surface, and the backend drawing the meshes. The sun's flare also counts as an
// effect draw, so draws without meshes are not a fault.
inline std::string CheckEffectCounts(const EffectCounts &counts)
{
    if (!counts.running && (counts.effects || counts.elems || counts.clouds || counts.surfaces || counts.meshes))
        return "effects were alive or drawn without the effects system running";
    if (counts.meshes > counts.surfaces)
        return "the effects generated " + std::to_string(counts.meshes) + " meshes but only " +
               std::to_string(counts.surfaces) + " effect draw surfaces";
    if (counts.surfaces > 0 && counts.effects == 0)
        return "the front end emitted " + std::to_string(counts.surfaces) + " effect surfaces with no effect alive";
    if (counts.surfaces > 0 && counts.elems == 0 && counts.clouds == 0)
        return "the front end emitted " + std::to_string(counts.surfaces) +
               " effect surfaces with no effect element alive";
    if (counts.meshes > 0 && counts.drawCalls == 0)
        return "the effects generated " + std::to_string(counts.meshes) +
               " meshes, but the backend issued no effect draw call";
    if (counts.drawCalls > 0 && counts.triangles == 0)
        return "the backend's effect draw calls drew no triangle";
    return {};
}

// Whether a frame shows effects: meshes and their draw surfaces.
inline bool EffectsDrawn(const EffectCounts &counts)
{
    return counts.meshes > 0 && counts.surfaces > 0;
}

// What every frame the free camera draws must agree on: the scene's static model
// and shadow counts, the effects system updated for the frame, and the effect
// counts.
inline std::string CheckFreeCameraCounts(const SceneCounts &scene, const EffectCounts &effects)
{
    if (std::string problem = CheckSceneCounts(scene); !problem.empty())
        return problem;
    if (!effects.running)
        return "the effects system was not updated for the frame";
    return CheckEffectCounts(effects);
}

// A switch that takes something out of the scene: the frame without it must say
// so in its counts and look different, and the frame with it back must be the one
// before. before/after are the settled digests with it on; without is the settled
// digest with it off.
inline std::string CheckSwitchedOff(const char *what, bool goneFromCounts, uint64_t before, uint64_t without,
                                    uint64_t after)
{
    if (!goneFromCounts)
        return std::string("with ") + what + " off, the renderer still counted them";
    if (without == before)
        return std::string("turning ") + what + " off did not change the picture";
    if (after != before)
        return std::string("turning ") + what + " back on did not bring the same picture back";
    return {};
}

// One try of a switch that need not change every picture it is tried at: an
// effect behind a wall is drawn and hidden by it. The counts must still say it
// is gone, and the picture with it back must be the one before; whether some
// try changed the picture is the caller's rule.
inline std::string CheckSwitchTry(const char *what, bool goneFromCounts, uint64_t before, uint64_t after)
{
    if (!goneFromCounts)
        return std::string("with ") + what + " off, the renderer still counted them";
    if (after != before)
        return std::string("turning ") + what + " back on did not bring the same picture back";
    return {};
}
} // namespace kisak_world_draw
