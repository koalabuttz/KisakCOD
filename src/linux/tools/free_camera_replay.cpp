// Free-camera replay: the free camera's FrameOwner over the engine renderer on a
// real map, driven by the free camera's two reference traversals with no window
// input. See world_draw_probe.h and docs/linux-free-camera.md.
//
// Every frame is the owner's: it steps the camera, asks the device, applies a
// pending resize and draws through EngineSceneRenderer. The replay only presses
// keys, moves the mouse and asks for frames, as a window host would, and reads
// each drawn frame back to measure it. It saves no image.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_init.h>
#include <ui/keycodes.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/view/frame_owner.h>

#include "engine_scene_renderer.h"
#include "world_draw_engine.h"
#include "world_draw_probe.h"
#include "world_draw_rules.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

extern int32_t g_zoneCount;

namespace
{
using namespace kisak_world_draw_engine;
namespace wdr = kisak_world_draw;
namespace kv = kisak::view;
using kisak_engine_view::EngineSceneRenderer;

// A checkpoint is drawn until it settles (wdr::CheckSettled), at most this many
// times; the world draw probe's rule and bound.
constexpr int kMaxSettleDraws = 64;
// Every frame is drawn at scene time 0, as the world draw probe draws, so a view
// drawn twice is comparable.
constexpr int kSceneTime = 0;
// Both sizes are 4:3, so the engine's aspect (r_aspectRatio auto) stays 4:3 and
// the camera's projection is the same at both.
constexpr kv::ViewExtent kLarger{800, 600};

struct Checkpoint
{
    std::string name;
    kv::ViewExtent extent;
    kv::CameraPose pose;
    std::vector<FrameResult> frames;
    std::vector<uint64_t> digests;
    bool settled = false;
    uint64_t Digest() const { return frames.back().stats.digest; }
};

struct Leg
{
    std::string name;
    unsigned frames = 0;
    double seconds = 0;
    double minLuma = 1e9, maxLuma = -1e9;
    size_t minColors = SIZE_MAX;
    // The leg's frame with the fewest colours and the pose it was drawn from: a
    // flat frame in motion is worth finding even when the checkpoints settle.
    int fewestColorsFrame = -1;
    kv::CameraPose fewestColorsPose{};
    FrameResult fewestColors{};
    // Where that frame's eye was: its visibility cell (R_CellForPoint, -1 for
    // none) and its collision contents (CM_PointContents).
    int fewestColorsCell = -1;
    int fewestColorsContents = 0;
    // Frames whose eye was inside solid collision, and frames with no world draw call.
    unsigned solidFrames = 0;
    unsigned noWorldFrames = 0;
};

struct Interruption
{
    std::string name;
    std::string checkpoint;
    uint64_t before = 0, after = 0;
};

kv::CameraPose PoseOf(const Camera &camera)
{
    kv::CameraPose pose;
    for (int i = 0; i < 3; ++i)
        pose.origin[i] = camera.eye[i], pose.angles[i] = camera.angles[i];
    return pose;
}

std::string ExtentText(kv::ViewExtent extent)
{
    return std::to_string(extent.width) + "x" + std::to_string(extent.height);
}

std::string PoseJson(const kv::CameraPose &pose)
{
    std::ostringstream json;
    json << "{\"origin\":[" << pose.origin[0] << "," << pose.origin[1] << "," << pose.origin[2] << "],\"angles\":["
         << pose.angles[0] << "," << pose.angles[1] << "," << pose.angles[2] << "]}";
    return json.str();
}

class Replay
{
public:
    Replay(EngineSceneRenderer &renderer, kv::FrameOwner &owner) : renderer_(renderer), owner_(owner) {}

    // One frame that must be drawn. Counted into the current leg.
    void Draw(int msec, Leg &leg)
    {
        const kv::FrameOutcome outcome = owner_.Frame(msec, kSceneTime);
        Require(outcome == kv::FrameOutcome::Drawn, "replay",
                leg.name + " frame " + std::to_string(leg.frames) + " was " + kv::FrameOutcomeName(outcome) +
                    (owner_.Error().empty() ? std::string() : ": " + owner_.Error()));
        const FrameResult &frame = renderer_.LastFrame();
        const kv::CameraPose &pose = owner_.Camera().Pose();
        const int contents = CM_PointContents(pose.origin, 0);
        if (contents & CONTENTS_SOLID)
            ++leg.solidFrames;
        if (frame.counts.worldDrawCalls == 0)
            ++leg.noWorldFrames;
        if (leg.fewestColorsFrame < 0 || frame.stats.distinctColors < leg.fewestColors.stats.distinctColors)
        {
            leg.fewestColorsFrame = int(leg.frames);
            leg.fewestColorsPose = pose;
            leg.fewestColors = frame;
            leg.fewestColorsCell = R_CellForPoint(rgp.world, pose.origin);
            leg.fewestColorsContents = contents;
        }
        ++leg.frames;
        leg.seconds += frame.seconds;
        leg.minLuma = std::min(leg.minLuma, frame.stats.meanLuma);
        leg.maxLuma = std::max(leg.maxLuma, frame.stats.meanLuma);
        leg.minColors = std::min(leg.minColors, frame.stats.distinctColors);
    }

    void Finish(Leg &leg)
    {
        const kv::CameraPose &pose = leg.fewestColorsPose;
        Say("free_camera_leg name=%s frames=%u seconds=%.3f luma=%.1f..%.1f min_colors=%zu solid_frames=%u "
            "no_world_frames=%u fewest_colors_frame=%d origin=%.2f,%.2f,%.2f angles=%.3f,%.3f,%.3f cell=%d "
            "contents=%s\n",
            leg.name.c_str(), leg.frames, leg.seconds, leg.minLuma, leg.maxLuma, leg.minColors, leg.solidFrames,
            leg.noWorldFrames, leg.fewestColorsFrame, pose.origin[0], pose.origin[1], pose.origin[2], pose.angles[0],
            pose.angles[1], pose.angles[2], leg.fewestColorsCell, Hex(uint32_t(leg.fewestColorsContents)).c_str());
        SayFrame((leg.name + ".fewest_colors").c_str(), leg.fewestColorsFrame, leg.fewestColors);
        legs_.push_back(leg);
    }

    // Draws the camera where it is, without moving it, until the view settles.
    const Checkpoint &Settle(const std::string &name)
    {
        Checkpoint checkpoint;
        checkpoint.name = name;
        checkpoint.pose = owner_.Camera().Pose();
        for (int draw = 0; draw < kMaxSettleDraws; ++draw)
        {
            const kv::FrameOutcome outcome = owner_.Frame(0, kSceneTime);
            Require(outcome == kv::FrameOutcome::Drawn, "replay",
                    name + " settle draw " + std::to_string(draw) + " was " + kv::FrameOutcomeName(outcome) +
                        (owner_.Error().empty() ? std::string() : ": " + owner_.Error()));
            checkpoint.frames.push_back(renderer_.LastFrame());
            checkpoint.digests.push_back(renderer_.LastFrame().stats.digest);
            SayFrame(name.c_str(), draw, checkpoint.frames.back());
            if (wdr::CheckSettled(checkpoint.digests, checkpoint.frames.back().smcPatches).empty())
                break;
        }
        checkpoint.extent = renderer_.Extent();
        checkpoint.settled = wdr::CheckSettled(checkpoint.digests, checkpoint.frames.back().smcPatches).empty();
        const kv::CameraPose &after = owner_.Camera().Pose();
        Require(std::equal(after.origin, after.origin + 3, checkpoint.pose.origin) &&
                    std::equal(after.angles, after.angles + 3, checkpoint.pose.angles),
                "replay", name + ": settling moved the camera");
        Say("free_camera_checkpoint name=%s extent=%s draws=%zu settled=%s digest=%s origin=%.2f,%.2f,%.2f "
            "angles=%.3f,%.3f,%.3f\n",
            name.c_str(), ExtentText(checkpoint.extent).c_str(), checkpoint.frames.size(),
            checkpoint.settled ? "yes" : "no", Hex64(checkpoint.Digest()).c_str(), checkpoint.pose.origin[0],
            checkpoint.pose.origin[1], checkpoint.pose.origin[2], checkpoint.pose.angles[0],
            checkpoint.pose.angles[1], checkpoint.pose.angles[2]);
        checkpoints_.push_back(std::move(checkpoint));
        return checkpoints_.back();
    }

    const Checkpoint &Find(const std::string &name) const
    {
        for (const Checkpoint &checkpoint : checkpoints_)
            if (checkpoint.name == name)
                return checkpoint;
        Fail("replay", "no checkpoint " + name);
    }

    void Interrupted(const std::string &name, const Checkpoint &before, const Checkpoint &after)
    {
        interruptions_.push_back({name, before.name, before.Digest(), after.Digest()});
        Say("free_camera_interruption name=%s checkpoint=%s before=%s after=%s same=%s\n", name.c_str(),
            before.name.c_str(), Hex64(before.Digest()).c_str(), Hex64(after.Digest()).c_str(),
            before.Digest() == after.Digest() ? "yes" : "no");
    }

    const std::deque<Checkpoint> &Checkpoints() const { return checkpoints_; }
    const std::vector<Leg> &Legs() const { return legs_; }
    const std::vector<Interruption> &Interruptions() const { return interruptions_; }

private:
    EngineSceneRenderer &renderer_;
    kv::FrameOwner &owner_;
    // A deque: a checkpoint stays where it is while later ones are added.
    std::deque<Checkpoint> checkpoints_;
    std::vector<Leg> legs_;
    std::vector<Interruption> interruptions_;
};

// The window host's part of a resize: the window takes the new size, and the
// owner hears it. A window manager may refuse the size; the back buffer follows
// the owner's extent either way, so this is reported, not required.
bool ResizeWindow(EngineSceneRenderer &renderer, kv::ViewExtent extent)
{
    SDL_Window *const window = static_cast<SDL_Window *>(renderer.Window());
    if (!window || !SDL_SetWindowSize(window, int(extent.width), int(extent.height)))
        return false;
    return SDL_SyncWindow(window);
}

void RequireLoaded(bool loaded, const kv::FrameOwner &owner, const char *what)
{
    Require(loaded, "replay", std::string(what) + ": " + owner.Error());
}
} // namespace

void Kisak_ReplayFreeCamera(const std::filesystem::path &directory, const char *mapName)
{
    const double startSeconds = WorldDrawStartSeconds();
    const auto started = std::chrono::steady_clock::now();
    RequireStartedRenderer();
    Say("free_camera_graphics_zones=loaded zones=%d seconds=%.2f\n", g_zoneCount, startSeconds);
    const kv::ViewExtent base{vidConfig.sceneWidth, vidConfig.sceneHeight};

    EngineSceneRenderer renderer(base, true);
    std::vector<std::string> problems;
    auto check = [&](const std::string &what, const std::string &problem) {
        if (!problem.empty())
            problems.push_back(what + ": " + problem);
    };
    Camera a, b;
    std::string worldName;
    bool windowResized = true;
    unsigned minimizedFrames = 0;
    kv::FrameCounters owned;
    // The owner shuts the renderer down when it goes, also after a failure; the
    // failure is printed first, so a fault in that shutdown cannot hide it.
    kv::FrameOwner owner(renderer, base);
    try
    {
        owner.SetAspect(vidConfig.aspectRatioWindow);
        owner.Focus(true);
        owner.SetLookCaptured(true);
        Replay replay(renderer, owner);

        // 1. The map, then its cameras. The owner takes a start pose only with a
        // load, so the first load finds the cameras and the second starts path A.
        const auto mapStarted = std::chrono::steady_clock::now();
        RequireLoaded(owner.Load(mapName), owner, "the first load");
        worldName = renderer.WorldName();
        Say("free_camera_map=loaded map=%s world=%s seconds=%.2f\n", mapName, worldName.c_str(), Seconds(mapStarted));
        ChooseCameras(*rgp.world, a, b);
        Say("free_camera_camera view=A classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", a.classname.c_str(),
            a.eye[0], a.eye[1], a.eye[2], a.angles[0], a.angles[1], a.angles[2]);
        Say("free_camera_camera view=B classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", b.classname.c_str(),
            b.eye[0], b.eye[1], b.eye[2], b.angles[0], b.angles[1], b.angles[2]);

        // 2. Path A (frame_owner_test.cpp TestReferenceTraversals) from camera A: one
        // second forward, a 90 degree mouse turn left, one fast second forward, then
        // half a second up.
        const kv::CameraPose startA = PoseOf(a);
        RequireLoaded(owner.Load(mapName, &startA), owner, "the load at camera A");
        replay.Settle("A.start");
        Leg forward{"A.forward"};
        owner.Key('w', true);
        for (int i = 0; i < 60; ++i)
            replay.Draw(1000 / 60 + (i % 3 == 0), forward);
        owner.Key('w', false);
        replay.Finish(forward);
        replay.Settle("A.forward");
        Leg turn{"A.turn"};
        owner.Mouse(-818, 0);
        replay.Draw(0, turn);
        replay.Finish(turn);
        replay.Settle("A.turn");
        Leg fast{"A.fast"};
        owner.Key('w', true);
        owner.Key(K_SHIFT, true);
        for (int i = 0; i < 10; ++i)
            replay.Draw(100, fast);
        owner.Key('w', false);
        owner.Key(K_SHIFT, false);
        replay.Finish(fast);
        replay.Settle("A.fast");
        Leg rise{"A.rise"};
        owner.Key(K_SPACE, true);
        for (int i = 0; i < 5; ++i)
            replay.Draw(100, rise);
        owner.Key(K_SPACE, false);
        replay.Finish(rise);
        const Checkpoint &end = replay.Settle("A.rise");
        const std::string endName = end.name;

        // 3. Interruptions where path A ended. Each must bring back the same picture.
        // A resize to 800x600 and back: two device resets at new sizes.
        windowResized = ResizeWindow(renderer, kLarger) && windowResized;
        owner.Resize(kLarger);
        replay.Settle("A.rise@" + ExtentText(kLarger));
        windowResized = ResizeWindow(renderer, base) && windowResized;
        owner.Resize(base);
        replay.Interrupted("resize", replay.Find(endName), replay.Settle("A.rise.resized"));
        // A minimize: no draw while the extent is empty, and no resize after it.
        owner.Resize({0, 0});
        const unsigned drawsBefore = renderer.Counters().draws;
        for (int i = 0; i < 3; ++i)
        {
            const kv::FrameOutcome outcome = owner.Frame(16, kSceneTime);
            Require(outcome == kv::FrameOutcome::Minimized, "replay",
                    std::string("a minimized frame was ") + kv::FrameOutcomeName(outcome));
            ++minimizedFrames;
        }
        Require(renderer.Counters().draws == drawsBefore, "replay", "a minimized frame drew");
        owner.Resize(base);
        replay.Interrupted("minimize", replay.Find(endName), replay.Settle("A.rise.restored"));
        // A device reset (DXVK never loses a windowed device, so the test asks).
        renderer.RequestReset();
        replay.Interrupted("device_reset", replay.Find(endName), replay.Settle("A.rise.reset"));
        // A reload of the map with the camera where it is.
        const kv::CameraPose beforeReload = owner.Camera().Pose();
        RequireLoaded(owner.Reload(), owner, "the reload");
        const kv::CameraPose &afterReload = owner.Camera().Pose();
        Require(std::equal(beforeReload.origin, beforeReload.origin + 3, afterReload.origin) &&
                    std::equal(beforeReload.angles, beforeReload.angles + 3, afterReload.angles),
                "replay", "the reload moved the camera");
        replay.Interrupted("reload", replay.Find(endName), replay.Settle("A.rise.reloaded"));

        // 4. Path B from camera B: an arrow-key square, each side one second forward
        // and 642 ms of turning left.
        const kv::CameraPose startB = PoseOf(b);
        RequireLoaded(owner.Load(mapName, &startB), owner, "the load at camera B");
        replay.Settle("B.start");
        Leg square{"B.square"};
        for (int side = 0; side < 4; ++side)
        {
            owner.Key('w', true);
            for (int i = 0; i < 5; ++i)
                replay.Draw(200, square);
            owner.Key('w', false);
            owner.Key(K_LEFTARROW, true);
            for (int i = 0; i < 3; ++i)
                replay.Draw(200, square);
            replay.Draw(42, square);
            owner.Key(K_LEFTARROW, false);
        }
        replay.Finish(square);
        replay.Settle("B.square");

        // 5. The rules on what was drawn.
        const uint32_t width = base.width, height = base.height;
        for (const char *name : {"A.start", "B.start"})
        {
            // The map's own cameras: the world draw probe's frame and count rules.
            const Checkpoint &checkpoint = replay.Find(name);
            check(name, wdr::CheckFrame(checkpoint.frames.back().stats, width, height));
            check(name, wdr::CheckDrawCounts(checkpoint.frames.back().counts));
        }
        for (const Checkpoint &checkpoint : replay.Checkpoints())
        {
            check(checkpoint.name, wdr::CheckSettled(checkpoint.digests, checkpoint.frames.back().smcPatches));
            const wdr::FrameStats &stats = checkpoint.frames.back().stats;
            if (stats.width != checkpoint.extent.width || stats.height != checkpoint.extent.height)
                check(checkpoint.name, "the back buffer is " + std::to_string(stats.width) + "x" +
                                           std::to_string(stats.height) + ", not " + ExtentText(checkpoint.extent));
        }
        check("path A", wdr::CheckViewsDiffer(replay.Find("A.start").Digest(), replay.Find(endName).Digest()));
        check("path B", wdr::CheckViewsDiffer(replay.Find("B.start").Digest(), replay.Find("B.square").Digest()));
        if (replay.Find("A.rise@" + ExtentText(kLarger)).extent != kLarger)
            check("resize", "the view was not drawn at " + ExtentText(kLarger));
        for (const Interruption &interruption : replay.Interruptions())
            if (interruption.before != interruption.after)
                check(interruption.name, "the settled picture after it (" + Hex64(interruption.after) +
                                             ") is not the one before it (" + Hex64(interruption.before) + ")");

        // 6. The report's view of the replay, then the owner's shutdown: the map's
        // unload, then the client's renderer shutdown.
        std::ostringstream body;
        body << ",\"checkpoints\":[";
        bool first = true;
        for (const Checkpoint &checkpoint : replay.Checkpoints())
        {
            body << (first ? "" : ",") << "\n{\"name\":" << Json(checkpoint.name) << ",\"extent\":["
                 << checkpoint.extent.width << "," << checkpoint.extent.height
                 << "],\"pose\":" << PoseJson(checkpoint.pose) << ",\"settled\":" << (checkpoint.settled ? "true" : "false")
                 << ",\"digest\":\"" << Hex64(checkpoint.Digest()) << "\",\"frames\":[";
            for (size_t i = 0; i < checkpoint.frames.size(); ++i)
                body << (i ? "," : "") << FrameJson(checkpoint.frames[i]);
            body << "]}";
            first = false;
        }
        body << "],\"legs\":[";
        first = true;
        for (const Leg &leg : replay.Legs())
        {
            char luma[64];
            std::snprintf(luma, sizeof(luma), "[%.2f,%.2f]", leg.minLuma, leg.maxLuma);
            body << (first ? "" : ",") << "{\"name\":" << Json(leg.name) << ",\"frames\":" << leg.frames
                 << ",\"seconds\":" << leg.seconds << ",\"mean_luma_range\":" << luma
                 << ",\"min_distinct_colors\":" << leg.minColors << ",\"solid_frames\":" << leg.solidFrames
                 << ",\"no_world_frames\":" << leg.noWorldFrames << ",\"fewest_colors\":{\"frame\":"
                 << leg.fewestColorsFrame << ",\"pose\":" << PoseJson(leg.fewestColorsPose)
                 << ",\"cell\":" << leg.fewestColorsCell << ",\"contents\":" << leg.fewestColorsContents
                 << ",\"result\":" << FrameJson(leg.fewestColors) << "}}";
            first = false;
        }
        body << "],\"interruptions\":[";
        first = true;
        for (const Interruption &interruption : replay.Interruptions())
        {
            body << (first ? "" : ",") << "{\"name\":" << Json(interruption.name)
                 << ",\"checkpoint\":" << Json(interruption.checkpoint) << ",\"before\":\""
                 << Hex64(interruption.before) << "\",\"after\":\"" << Hex64(interruption.after)
                 << "\",\"same\":" << (interruption.before == interruption.after ? "true" : "false") << "}";
            first = false;
        }
        body << "]";

        owner.Shutdown();
        Require(owner.State() == kv::FrameState::Stopped, "shutdown",
                std::string("the owner is ") + kv::FrameStateName(owner.State()) + " after its shutdown");
        owned = owner.Counters();

        // 7. The lifecycle rules.
        const kisak_engine_view::EngineCounters &engine = renderer.Counters();
        check("owner", owner.Error());
        check("renderer", renderer.Problem());
        if (owned.loads != 4 || owned.loadFailures != 0 || owned.unloads != 4)
            check("lifecycle", "expected 4 loads, no failed load and 4 unloads; the owner counted " +
                                   std::to_string(owned.loads) + ", " + std::to_string(owned.loadFailures) + " and " +
                                   std::to_string(owned.unloads));
        if (engine.loads != owned.loads || engine.unloads != owned.unloads)
            check("lifecycle", "the renderer counted " + std::to_string(engine.loads) + " loads and " +
                                   std::to_string(engine.unloads) + " unloads");
        if (owned.resizes != 2 || engine.resizes != 2 || owned.resets != 1 || engine.resets != 1 ||
            owned.resetRetries != 0)
            check("lifecycle", "expected 2 resizes and 1 device reset; the owner counted " +
                                   std::to_string(owned.resizes) + " and " + std::to_string(owned.resets) +
                                   ", the renderer " + std::to_string(engine.resizes) + " and " +
                                   std::to_string(engine.resets));
        if (engine.emptyFrames != 1 + engine.resizes + engine.resets)
            check("lifecycle", "expected one empty frame after R_Init and after each reset, counted " +
                                   std::to_string(engine.emptyFrames));
        if (owned.draws != engine.draws)
            check("lifecycle", "the owner counted " + std::to_string(owned.draws) + " draws, the renderer " +
                                   std::to_string(engine.draws));
        if (!renderer.ShutDown() || dx.device || dx.d3d9 || rg.registered || MapHeld() || g_zoneCount != 0)
            check("shutdown", "the renderer still holds its device, registration, a map or a zone (zones=" +
                                  std::to_string(g_zoneCount) + ")");

        const double seconds = Seconds(started);
        Say("free_camera_counters loads=%u unloads=%u resizes=%u resets=%u draws=%u empty_frames=%u frames=%u "
            "minimized_frames=%u\n",
            owned.loads, owned.unloads, owned.resizes, owned.resets, owned.draws, engine.emptyFrames, owned.frames,
            minimizedFrames);
        Say("free_camera_shutdown=done device=%s zones=%d window_resized=%s\n", dx.device ? "held" : "released",
            g_zoneCount, windowResized ? "yes" : "no");

        // 8. The report: the cameras, checkpoints, counts and measurements. No pixels.
        std::ostringstream json;
        json << "{\"schema\":1,\"tool\":\"free_camera_replay\",\"map\":" << Json(mapName)
             << ",\"world\":" << Json(worldName) << ",\"renderer\":\"shader_model_3\",\"window\":[" << base.width
             << "," << base.height << "],\"larger\":[" << kLarger.width << "," << kLarger.height
             << "],\"scene_time\":" << kSceneTime << ",\"cameras\":{\"A\":" << CameraJson(a)
             << ",\"B\":" << CameraJson(b) << "}" << body.str() << ",\"owner\":{\"frames\":" << owned.frames
             << ",\"draws\":" << owned.draws << ",\"loads\":" << owned.loads << ",\"unloads\":" << owned.unloads
             << ",\"resizes\":" << owned.resizes << ",\"resets\":" << owned.resets
             << ",\"minimized_frames\":" << minimizedFrames << "},\"renderer\":{\"draws\":" << engine.draws
             << ",\"empty_frames\":" << engine.emptyFrames << ",\"resizes\":" << engine.resizes
             << ",\"resets\":" << engine.resets << "},\"window_resized\":" << (windowResized ? "true" : "false")
             << ",\"max_settle_draws\":" << kMaxSettleDraws << ",\"start_seconds\":" << startSeconds
             << ",\"seconds\":" << seconds << ",\"problem_count\":" << problems.size() << ",\"problems\":[";
        for (size_t i = 0; i < problems.size(); ++i)
            json << (i ? "," : "") << "\n" << Json(problems[i]);
        json << "]}\n";
        WriteExclusiveFile(directory / "free-camera-replay.json", json.str());

        for (const std::string &problem : problems)
            Say("free_camera_problem %s\n", problem.c_str());
        Say("free_camera_replay=%s problems=%zu seconds=%.2f\n", problems.empty() ? "passed" : "failed",
            problems.size(), seconds);
    }
    catch (const std::exception &failure)
    {
        Say("free_camera_failure %s\n", failure.what());
        throw;
    }
    Require(problems.empty(), "check",
            std::to_string(problems.size()) + " replay rule(s) failed (see free_camera_problem lines)");
}
