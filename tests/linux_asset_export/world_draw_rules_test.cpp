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
    Passes(wdr::CheckSettled({1, 2, 2}, 0), "a view whose last two draws match with the cache full");
    Fails(wdr::CheckSettled({1, 2, 2}, 256), "a view whose pixels match while the cache still fills");
    Fails(wdr::CheckSettled({1, 2, 3}, 0), "a view whose last two draws differ, cache full or not");
    Fails(wdr::CheckSettled({7}, 0), "a view drawn once");
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

wdr::SceneCounts SunlitViewWithModels()
{
    wdr::SceneCounts counts;
    counts.staticModels = 1200;
    counts.staticModelsDrawn = 310;
    counts.staticModelSurfaces = 420;
    counts.staticModelDrawCalls = 150;
    counts.staticModelTriangles = 90000;
    counts.sunLight = counts.sunUsed = counts.shadowMaps = counts.sunShadowDrawn = true;
    counts.sunShadowSurfaces = 600;
    counts.sunShadowStaticModelSurfaces = 250;
    return counts;
}

void CheckSceneCounts()
{
    const wdr::SceneCounts view = SunlitViewWithModels();
    Passes(wdr::CheckSceneCounts(view), "a sunlit view with static models");
    Passes(wdr::CheckSceneInView(view), "a map camera's sunlit view with static models");

    wdr::SceneCounts indoors = view;
    indoors.sunUsed = indoors.sunShadowDrawn = false;
    indoors.sunShadowSurfaces = indoors.sunShadowStaticModelSurfaces = 0;
    Passes(wdr::CheckSceneCounts(indoors), "a view the sun does not light");
    Fails(wdr::CheckSceneInView(indoors), "a map camera with no sun shadow in a world with a sun");
    wdr::SceneCounts noSun = indoors;
    noSun.sunLight = false;
    Passes(wdr::CheckSceneInView(noSun), "a map camera in a world without a sun");
    wdr::SceneCounts shadowsOff = view;
    shadowsOff.shadowMaps = shadowsOff.sunShadowDrawn = false;
    shadowsOff.sunShadowSurfaces = shadowsOff.sunShadowStaticModelSurfaces = 0;
    Passes(wdr::CheckSceneCounts(shadowsOff), "a sunlit view with shadow maps off");

    wdr::SceneCounts none = view;
    none.staticModelsDrawn = none.staticModelSurfaces = none.staticModelDrawCalls = none.staticModelTriangles = 0;
    Passes(wdr::CheckSceneCounts(none), "a view with no static model in it");
    Fails(wdr::CheckSceneInView(none), "a map camera that drew none of the world's static models");
    wdr::SceneCounts noModels = none;
    noModels.staticModels = 0;
    Passes(wdr::CheckSceneInView(noModels), "a map camera in a world without static models");

    wdr::SceneCounts tooMany = view;
    tooMany.staticModelsDrawn = view.staticModels + 1;
    Fails(wdr::CheckSceneCounts(tooMany), "more static models kept than the world has");
    wdr::SceneCounts noSurfaces = view;
    noSurfaces.staticModelSurfaces = 0;
    Fails(wdr::CheckSceneCounts(noSurfaces), "static models kept with no surface");
    wdr::SceneCounts noCalls = view;
    noCalls.staticModelDrawCalls = 0;
    Fails(wdr::CheckSceneCounts(noCalls), "static model surfaces the backend did not draw");
    wdr::SceneCounts noTriangles = view;
    noTriangles.staticModelTriangles = 0;
    Fails(wdr::CheckSceneCounts(noTriangles), "static model draw calls without triangles");
    wdr::SceneCounts strangeSun = view;
    strangeSun.sunLight = false;
    Fails(wdr::CheckSceneCounts(strangeSun), "a sun in use in a world without one");
    wdr::SceneCounts skipped = view;
    skipped.sunShadowDrawn = false;
    skipped.sunShadowSurfaces = skipped.sunShadowStaticModelSurfaces = 0;
    Fails(wdr::CheckSceneCounts(skipped), "a sunlit view whose shadow map was skipped");
    wdr::SceneCounts unwanted = shadowsOff;
    unwanted.sunShadowDrawn = true;
    unwanted.sunShadowSurfaces = 10;
    Fails(wdr::CheckSceneCounts(unwanted), "a sun shadow map drawn with shadow maps off");
    wdr::SceneCounts empty = view;
    empty.sunShadowSurfaces = empty.sunShadowStaticModelSurfaces = 0;
    Fails(wdr::CheckSceneCounts(empty), "an empty sun shadow map");
    wdr::SceneCounts moreModels = view;
    moreModels.sunShadowStaticModelSurfaces = view.sunShadowSurfaces + 1;
    Fails(wdr::CheckSceneCounts(moreModels), "more static model shadow surfaces than shadow surfaces");

    Passes(wdr::CheckSwitchedOff("static models", true, 1, 2, 1), "a switch that takes something visible out");
    Fails(wdr::CheckSwitchedOff("static models", false, 1, 2, 1), "a switch the counts do not follow");
    Fails(wdr::CheckSwitchedOff("static models", true, 1, 1, 1), "a switch that changes nothing on screen");
    Fails(wdr::CheckSwitchedOff("static models", true, 1, 2, 3), "a switch that does not come back");

    Passes(wdr::CheckSwitchTry("effects", true, 1, 1), "a try whose effects were hidden behind a wall");
    Fails(wdr::CheckSwitchTry("effects", false, 1, 1), "a try the counts do not follow");
    Fails(wdr::CheckSwitchTry("effects", true, 1, 3), "a try that does not come back");
}
void CheckEffectCounts()
{
    wdr::EffectCounts view;
    view.running = true;
    view.effects = 40;
    view.elems = 900;
    view.surfaces = 30;
    view.meshes = 28;
    view.drawCalls = 31;
    view.triangles = 1800;
    Passes(wdr::CheckEffectCounts(view), "a frame with effects in view");
    Check(wdr::EffectsDrawn(view), "a frame with effect meshes and surfaces shows effects");
    wdr::EffectCounts away = view;
    away.surfaces = away.meshes = 0;
    away.drawCalls = 1;
    away.triangles = 2;
    Passes(wdr::CheckEffectCounts(away), "effects alive out of view, and the sun's flare drawn");
    Check(!wdr::EffectsDrawn(away), "a frame without effect meshes shows none");
    wdr::EffectCounts drawMode;
    drawMode.drawCalls = 1;
    drawMode.triangles = 2;
    Passes(wdr::CheckEffectCounts(drawMode), "the draw mode: no effects system, the sun's flare drawn");

    wdr::EffectCounts stray = view;
    stray.running = false;
    Fails(wdr::CheckEffectCounts(stray), "effects drawn without the system running");
    wdr::EffectCounts lostSurfaces = view;
    lostSurfaces.surfaces = lostSurfaces.meshes - 1;
    Fails(wdr::CheckEffectCounts(lostSurfaces), "more effect meshes than effect surfaces");
    wdr::EffectCounts orphan = view;
    orphan.effects = 0;
    Fails(wdr::CheckEffectCounts(orphan), "effect surfaces with no effect alive");
    wdr::EffectCounts empty = view;
    empty.elems = 0;
    Fails(wdr::CheckEffectCounts(empty), "effect surfaces with no element alive");
    wdr::EffectCounts clouds = empty;
    clouds.clouds = 3;
    Passes(wdr::CheckEffectCounts(clouds), "effect surfaces from clouds alone");
    wdr::EffectCounts undrawn = view;
    undrawn.drawCalls = undrawn.triangles = 0;
    Fails(wdr::CheckEffectCounts(undrawn), "effect meshes the backend never drew");
    wdr::EffectCounts flat = view;
    flat.triangles = 0;
    Fails(wdr::CheckEffectCounts(flat), "effect draw calls with no triangle");
}

// A frame with the map's vision set applied: glow and film asked for, allowed and
// used, and the post effects run for them.
wdr::PostCounts VisionFrame()
{
    wdr::PostCounts post;
    post.vision = true;
    post.glowAsked = post.filmAsked = true;
    post.glowSwitch = post.filmSwitch = true;
    post.glowUsed = post.filmUsed = post.postEffects = true;
    return post;
}

void CheckPostCounts()
{
    // The engine's tests of whether a setting does anything.
    const float ones[3] = {1.0f, 1.0f, 1.0f};
    const float warm[3] = {1.1f, 1.0f, 0.9f};
    Check(wdr::GlowAsked(true, 0.5f, 4.0f), "glow on with a bloom and a radius is asked for");
    Check(!wdr::GlowAsked(false, 0.5f, 4.0f), "glow off is not asked for");
    Check(!wdr::GlowAsked(true, 0.0f, 4.0f), "glow without a bloom is not asked for");
    Check(!wdr::GlowAsked(true, 0.5f, 0.0f), "glow without a radius is not asked for");
    Check(!wdr::FilmAsked(true, 1.0f, 0.0f, 0.0f, false, ones, ones), "film that changes no colour is not asked for");
    Check(!wdr::FilmAsked(false, 1.4f, 0.0f, 0.0f, false, ones, ones), "film off is not asked for");
    Check(wdr::FilmAsked(true, 1.4f, 0.0f, 0.0f, false, ones, ones), "film with a contrast is asked for");
    Check(wdr::FilmAsked(true, 1.0f, 0.1f, 0.0f, false, ones, ones), "film with a brightness is asked for");
    Check(wdr::FilmAsked(true, 1.0f, 0.0f, 0.2f, false, ones, ones), "film with a desaturation is asked for");
    Check(wdr::FilmAsked(true, 1.0f, 0.0f, 0.0f, true, ones, ones), "an inverting film is asked for");
    Check(wdr::FilmAsked(true, 1.0f, 0.0f, 0.0f, false, warm, ones), "film with a dark tint is asked for");
    Check(wdr::FilmAsked(true, 1.0f, 0.0f, 0.0f, false, ones, warm), "film with a light tint is asked for");

    const wdr::PostCounts vision = VisionFrame();
    Passes(wdr::CheckPostCounts(vision), "glow and film asked for, allowed and used");
    wdr::PostCounts plain;
    plain.vision = true;
    plain.glowSwitch = plain.filmSwitch = true;
    Passes(wdr::CheckPostCounts(plain), "a vision set that asks for neither, and no post effect");
    wdr::PostCounts glowOff = vision;
    glowOff.glowSwitch = false;
    glowOff.glowUsed = false;
    Passes(wdr::CheckPostCounts(glowOff), "glow switched off, film still used");
    wdr::PostCounts filmOff = vision;
    filmOff.filmSwitch = false;
    filmOff.filmUsed = false;
    Passes(wdr::CheckPostCounts(filmOff), "film switched off, glow still used");

    wdr::PostCounts none = vision;
    none.vision = false;
    Fails(wdr::CheckPostCounts(none), "a frame drawn without the vision set");
    wdr::PostCounts lostGlow = vision;
    lostGlow.glowUsed = false;
    Fails(wdr::CheckPostCounts(lostGlow), "glow asked for and allowed but not used");
    wdr::PostCounts strayGlow = glowOff;
    strayGlow.glowUsed = true;
    Fails(wdr::CheckPostCounts(strayGlow), "glow used while switched off");
    wdr::PostCounts lostFilm = vision;
    lostFilm.filmUsed = false;
    Fails(wdr::CheckPostCounts(lostFilm), "film asked for and allowed but not used");
    wdr::PostCounts strayFilm = plain;
    strayFilm.filmUsed = strayFilm.postEffects = true;
    Fails(wdr::CheckPostCounts(strayFilm), "film used that the vision set does not ask for");
    wdr::PostCounts extra = plain;
    extra.postEffects = true;
    Fails(wdr::CheckPostCounts(extra), "post effects run for neither glow nor film");
    wdr::PostCounts skipped = vision;
    skipped.postEffects = false;
    Fails(wdr::CheckPostCounts(skipped), "glow and film used with no post effect run");
}

void CheckFreeCameraCounts()
{
    wdr::EffectCounts view;
    view.running = true;
    view.effects = 4;
    view.elems = 30;
    view.surfaces = 12;
    view.meshes = 12;
    view.drawCalls = 13;
    view.triangles = 60;
    wdr::EffectCounts away;
    away.running = true;
    away.effects = 4;
    away.elems = 30;
    away.drawCalls = 1;
    away.triangles = 2;
    wdr::EffectCounts drawMode;
    drawMode.drawCalls = 1;
    drawMode.triangles = 2;
    wdr::EffectCounts undrawn = view;
    undrawn.drawCalls = undrawn.triangles = 0;
    const wdr::PostCounts post = VisionFrame();

    // The free camera's frame rule: the scene's counts first, then the effects,
    // then the vision set's post effects.
    wdr::SceneCounts scene;
    Passes(wdr::CheckFreeCameraCounts(scene, view, post), "a free-camera frame with effects in view");
    Passes(wdr::CheckFreeCameraCounts(scene, away, post), "a free-camera frame with effects out of view");
    Fails(wdr::CheckFreeCameraCounts(scene, drawMode, post),
          "a free-camera frame without the effects system updated");
    Fails(wdr::CheckFreeCameraCounts(scene, undrawn, post),
          "a free-camera frame whose effect meshes were never drawn");
    wdr::SceneCounts strayModels;
    strayModels.staticModelsDrawn = 3;
    Fails(wdr::CheckFreeCameraCounts(strayModels, view, post), "a free-camera frame whose scene counts disagree");
    wdr::PostCounts noVision = post;
    noVision.vision = false;
    Fails(wdr::CheckFreeCameraCounts(scene, view, noVision), "a free-camera frame without the vision set");
}
} // namespace

int main()
{
    CheckMeasurement();
    CheckFrames();
    CheckRepeatsAndViews();
    CheckCounts();
    CheckSceneCounts();
    CheckEffectCounts();
    CheckPostCounts();
    CheckFreeCameraCounts();
    if (failures)
    {
        std::printf("world_draw_rules_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_draw_rules_test: frame measurement and rules ok\n");
    return 0;
}
