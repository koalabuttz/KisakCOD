// Asset-free checks for the world draw probe's frame rules.
//
// The probe itself needs a GPU, a packaged map and the whole renderer, so it
// cannot run here, and no drawn frame may be stored to replay. What can run here
// is everything that decides whether a frame passes: the measurement in
// world_draw_rules.h is checked on frames built in memory with known content,
// and each rule is shown to accept a frame shaped like a drawn view and to
// reject one with exactly one thing wrong.
//
// The test links no engine source; it includes one header.
//
// Build and run (scripts/tests/run_world_surface_checks.py runs it as
// world_draw_rules):
//   clang++ -m32 -std=c++20 -I src tests/linux_asset_export/world_draw_rules_test.cpp -o /tmp/wdr_test
//   /tmp/wdr_test

#include <linux/tools/world_draw_rules.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
namespace wdr = kisak_world_draw;

int failures = 0;

void Check(bool condition, const std::string &what)
{
    if (condition)
        return;
    std::printf("FAIL %s\n", what.c_str());
    ++failures;
}

void Passes(const std::string &problem, const std::string &what)
{
    Check(problem.empty(), what + " passes (got: " + problem + ")");
}

void Fails(const std::string &problem, const std::string &what)
{
    Check(!problem.empty(), what + " fails");
}

// A frame in the layout the D3D9 X8R8G8B8 back buffer has: 4 bytes a pixel,
// blue first, with optional padding at the end of each row.
struct Frame
{
    uint32_t width = 0, height = 0;
    size_t pitch = 0;
    std::vector<uint8_t> bytes;

    Frame(uint32_t w, uint32_t h, size_t padding = 0) : width(w), height(h), pitch(size_t(w) * 4 + padding)
    {
        bytes.assign(pitch * h, 0xAB); // padding keeps a pattern the rules must ignore
    }
    void Set(uint32_t x, uint32_t y, uint32_t rgb, uint8_t fourth = 0xFF)
    {
        uint8_t *p = &bytes[size_t(y) * pitch + size_t(x) * 4];
        p[0] = uint8_t(rgb), p[1] = uint8_t(rgb >> 8), p[2] = uint8_t(rgb >> 16), p[3] = fourth;
    }
    wdr::FrameStats Measure() const { return wdr::MeasureFrame(bytes.data(), width, height, pitch); }
};

// A deterministic stand-in for a lit, textured view: a smooth gradient with
// noise on top, every tile different, mid brightness.
uint32_t Textured(uint32_t x, uint32_t y, uint32_t &seed)
{
    seed = seed * 1664525u + 1013904223u;
    const uint32_t noise = (seed >> 24) & 0x1F;
    const uint32_t r = (40 + x / 4 + noise) & 0xFF;
    const uint32_t g = (60 + y / 3 + noise) & 0xFF;
    const uint32_t b = (90 + (x + y) / 8 + (noise >> 1)) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

Frame View(uint32_t w = 640, uint32_t h = 480, size_t padding = 0, uint32_t seed = 7)
{
    Frame frame(w, h, padding);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            frame.Set(x, y, Textured(x, y, seed));
    return frame;
}

void CheckMeasurement()
{
    Frame cleared(64, 48);
    for (uint32_t y = 0; y < 48; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            cleared.Set(x, y, 0x204060);
    const wdr::FrameStats c = cleared.Measure();
    Check(c.width == 64 && c.height == 48, "the measured size is the frame's");
    Check(c.distinctColors == 1, "a cleared frame has one colour");
    Check(c.dominantShare == 1.0 && c.dominantColor == 0x204060, "a cleared frame's colour covers all of it");
    Check(c.detailTiles == 0, "a cleared frame has no tile with detail");
    Check(c.lumaStdDev == 0.0, "a cleared frame has no luma spread");
    const double luma = wdr::Luma(0x204060);
    Check(c.meanLuma > luma - 1e-9 && c.meanLuma < luma + 1e-9, "the mean luma of one colour is its luma");

    // Two colours split down the middle: half each, detail only in the tiles the
    // split runs through (none here, since 64 / 4 puts the split on a tile edge).
    Frame split(64, 48);
    for (uint32_t y = 0; y < 48; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            split.Set(x, y, x < 32 ? 0x000000 : 0xFFFFFF);
    const wdr::FrameStats s = split.Measure();
    Check(s.distinctColors == 2 && s.dominantShare == 0.5, "a half-and-half frame has two colours at one half each");
    Check(s.dominantColor == 0x000000, "a tie goes to the lower colour value");
    Check(s.detailTiles == 0, "a split on a tile edge leaves every tile flat");
    Check(s.meanLuma > 127.4 && s.meanLuma < 127.6, "black and white halves average to mid grey");

    // The fourth byte (alpha or unused) and the row padding are not part of the image.
    Frame a = View(37, 23, 0, 3), b = View(37, 23, 12, 3);
    for (uint32_t y = 0; y < 23; ++y)
        for (uint32_t x = 0; x < 37; ++x)
        {
            const uint8_t *p = &b.bytes[size_t(y) * b.pitch + size_t(x) * 4];
            b.Set(x, y, (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0], uint8_t(x * 7 + y));
        }
    Check(a.Measure().digest == b.Measure().digest, "row padding and the fourth byte do not change the digest");
    Check(a.Measure().distinctColors == b.Measure().distinctColors, "row padding adds no colour");
    // One pixel's colour does.
    Frame c2 = View(37, 23, 0, 3);
    c2.Set(36, 22, 0x123456);
    Check(c2.Measure().digest != a.Measure().digest, "one changed pixel changes the digest");
    // Tiles cover frames whose size is not a multiple of four.
    Check(View(641, 479).Measure().detailTiles == wdr::kTileCount, "every tile of an odd-sized view has detail");
    Check(wdr::MeasureFrame(nullptr, 0, 0, 0).distinctColors == 0, "an empty frame measures nothing");
}

void CheckFrames()
{
    const wdr::FrameStats view = View().Measure();
    Check(view.distinctColors >= wdr::kMinDistinctColors, "the synthetic view has many colours");
    Passes(wdr::CheckFrame(view, 640, 480), "a textured, lit view");
    Fails(wdr::CheckFrame(view, 800, 600), "a frame the size of another window");

    Frame cleared(640, 480);
    for (uint32_t y = 0; y < 480; ++y)
        for (uint32_t x = 0; x < 640; ++x)
            cleared.Set(x, y, 0x000000);
    Fails(wdr::CheckFrame(cleared.Measure(), 640, 480), "a cleared frame");

    // Sky filling 60% of the view as one flat colour.
    Frame flat = View();
    for (uint32_t y = 0; y < 288; ++y)
        for (uint32_t x = 0; x < 640; ++x)
            flat.Set(x, y, 0x5080C0);
    const wdr::FrameStats flatStats = flat.Measure();
    Check(flatStats.distinctColors >= wdr::kMinDistinctColors, "a view with a flat top still has many colours");
    Fails(wdr::CheckFrame(flatStats, 640, 480), "a view with one colour over 60% of it");

    // The left half drawn as eight flat tiles of different colours, the right half
    // textured: many colours, no dominant one, but detail in half the tiles.
    Frame half = View();
    for (uint32_t y = 0; y < 480; ++y)
        for (uint32_t x = 0; x < 320; ++x)
            half.Set(x, y, 0x101010u * (1 + (y * 4 / 480) * 2 + x * 4 / 640));
    const wdr::FrameStats halfStats = half.Measure();
    Check(halfStats.detailTiles == wdr::kTileCount / 2, "flat tiles on the left have no detail");
    Check(halfStats.dominantShare <= wdr::kMaxDominantShare, "no flat tile dominates");
    Fails(wdr::CheckFrame(halfStats, 640, 480), "a view with detail in only half its tiles");

    // Many colours, but all of them nearly black or nearly white.
    Frame dark(640, 480), bright(640, 480);
    uint32_t seed = 11;
    for (uint32_t y = 0; y < 480; ++y)
        for (uint32_t x = 0; x < 640; ++x)
        {
            seed = seed * 1664525u + 1013904223u;
            const uint32_t low = ((seed >> 8) & 7) << 16 | ((seed >> 12) & 7) << 8 | ((seed >> 16) & 7);
            dark.Set(x, y, low);
            bright.Set(x, y, 0xF8F8F8 | low);
        }
    const wdr::FrameStats darkStats = dark.Measure(), brightStats = bright.Measure();
    Check(darkStats.distinctColors >= wdr::kMinDistinctColors && darkStats.dominantShare <= wdr::kMaxDominantShare &&
              darkStats.detailTiles == wdr::kTileCount,
          "the dark frame breaks only the brightness rule");
    Fails(wdr::CheckFrame(darkStats, 640, 480), "a frame too dark to be a lit view");
    Fails(wdr::CheckFrame(brightStats, 640, 480), "a frame too bright to be a lit view");
}

void CheckRepeatsAndViews()
{
    Passes(wdr::CheckRepeatable({1, 2, 2}), "a view whose first draw differs and whose last two match");
    Passes(wdr::CheckRepeatable({5, 5}), "two equal draws");
    Fails(wdr::CheckRepeatable({1, 2, 3}), "a view whose last two draws differ");
    Fails(wdr::CheckRepeatable({4}), "a view drawn once");
    Fails(wdr::CheckRepeatable({}), "a view never drawn");
    Passes(wdr::CheckViewsDiffer(1, 2), "two cameras with different pixels");
    Fails(wdr::CheckViewsDiffer(3, 3), "two cameras with the same pixels");
}

void CheckCounts()
{
    wdr::DrawCounts counts;
    counts.litSurfaces = 900;
    counts.decalSurfaces = 40;
    counts.emissiveSurfaces = 3;
    counts.worldDrawCalls = 350;
    counts.worldTriangles = 120000;
    Passes(wdr::CheckDrawCounts(counts), "a frame whose world was emitted and drawn");
    wdr::DrawCounts emissiveOnly{};
    emissiveOnly.emissiveSurfaces = 1, emissiveOnly.worldDrawCalls = 1, emissiveOnly.worldTriangles = 2;
    Passes(wdr::CheckDrawCounts(emissiveOnly), "a frame with only emissive surfaces");
    wdr::DrawCounts none = counts;
    none.litSurfaces = none.decalSurfaces = none.emissiveSurfaces = 0;
    Fails(wdr::CheckDrawCounts(none), "a frame with no surface emitted");
    wdr::DrawCounts noCalls = counts;
    noCalls.worldDrawCalls = 0;
    Fails(wdr::CheckDrawCounts(noCalls), "a frame whose backend drew no world");
    wdr::DrawCounts noTriangles = counts;
    noTriangles.worldTriangles = 0;
    Fails(wdr::CheckDrawCounts(noTriangles), "world draw calls without triangles");
}
} // namespace

int main()
{
    CheckMeasurement();
    CheckFrames();
    CheckRepeatsAndViews();
    CheckCounts();
    if (failures)
    {
        std::printf("world_draw_rules_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_draw_rules_test: frame measurement and rules ok\n");
    return 0;
}
