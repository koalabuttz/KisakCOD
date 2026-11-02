// Free-camera window driver: the free camera flown through the engine renderer's
// own window. See world_draw_probe.h and docs/linux-free-camera.md, "The window
// driver".
//
// Three parts meet here and nowhere else:
//   * EngineSceneRenderer (engine_scene_renderer.h), the renderer side, which made
//     the window in R_Init and destroys it in its Shutdown;
//   * FrameOwner (src/linux/client/view/frame_owner.h), which owns every frame;
//   * FreeCameraWindow (src/linux/client/view/free_camera_window.h), the only
//     event pump, which adopts that window and feeds its input to the owner.
// The order is fixed: the window is adopted after the renderer has made it, and
// FreeCameraWindow is destroyed before FrameOwner::Shutdown, because that
// shutdown destroys the window it borrows.
//
// KISAK_FREE_CAMERA_INPUT=window flies until the window quits (Escape twice, or
// closing it) or KISAK_FREE_CAMERA_SECONDS run out. KISAK_FREE_CAMERA_INPUT=script
// drives the same loop with SDL events pushed into the window's own queue, and
// checks what each one did against a second owner fed the same input directly.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_init.h>
#include <ui/keycodes.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/view/frame_owner.h>
#include <linux/client/view/free_camera_window.h>

#include "engine_scene_renderer.h"
#include "world_draw_engine.h"
#include "world_draw_probe.h"
#include "world_draw_rules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

extern int32_t g_zoneCount;

namespace
{
using namespace kisak_world_draw_engine;
namespace wdr = kisak_world_draw;
namespace kv = kisak::view;
namespace sdl = kisak::sdl;
using kisak_engine_view::EngineSceneRenderer;

// As the replay: a checkpoint is drawn until it settles, at most this many times,
// and the scripted run draws at scene time 0 so that a view drawn twice compares.
constexpr int kMaxSettleDraws = 64;
constexpr int kSceneTime = 0;
// How long the scripted run waits for the window system to apply a size, a
// minimize or a restore before it records that it did not.
constexpr int kWindowWaitMilliseconds = 3000;
// Every event the script pushes carries this device id, so an input event that
// did not come from the script can be told apart and counted.
constexpr uint32_t kScriptDevice = 0x4b495341u;
// A live frame never waits for events; a minimized window, or a lost device,
// waits this long for the next one, so the loop does not spin.
constexpr int kIdleWaitMilliseconds = 50;

kv::CameraPose PoseOf(const Camera &camera)
{
    kv::CameraPose pose;
    for (int i = 0; i < 3; ++i)
        pose.origin[i] = camera.eye[i], pose.angles[i] = camera.angles[i];
    return pose;
}

bool SamePose(const kv::CameraPose &a, const kv::CameraPose &b)
{
    return std::equal(a.origin, a.origin + 3, b.origin) && std::equal(a.angles, a.angles + 3, b.angles);
}

std::string ExtentText(kv::ViewExtent extent)
{
    return std::to_string(extent.width) + "x" + std::to_string(extent.height);
}

std::string PoseText(const kv::CameraPose &pose)
{
    char text[160];
    std::snprintf(text, sizeof(text), "origin=%.3f,%.3f,%.3f angles=%.4f,%.4f,%.4f", pose.origin[0], pose.origin[1],
                  pose.origin[2], pose.angles[0], pose.angles[1], pose.angles[2]);
    return text;
}

std::string PoseJson(const kv::CameraPose &pose)
{
    std::ostringstream json;
    json << "{\"origin\":[" << pose.origin[0] << "," << pose.origin[1] << "," << pose.origin[2] << "],\"angles\":["
         << pose.angles[0] << "," << pose.angles[1] << "," << pose.angles[2] << "]}";
    return json.str();
}

// The engine renderer with one addition: after each resize the owner takes the
// aspect the engine chose for the new size (R_StoreWindowSettings snaps
// r_aspectRatio auto to 4:3, 16:10 or 16:9). The owner asks for the resize at
// the start of a frame and builds that frame's view after it, so the first frame
// at a new size already has the engine's aspect.
class AspectFollower final : public kv::SceneRenderer
{
public:
    explicit AspectFollower(EngineSceneRenderer &renderer) : renderer_(renderer) {}
    void Attach(kv::FrameOwner &owner)
    {
        owner_ = &owner;
        Apply();
    }

    bool LoadMap(const std::string &map, std::string &error) override { return renderer_.LoadMap(map, error); }
    void UnloadMap() noexcept override { renderer_.UnloadMap(); }
    bool Resize(kv::ViewExtent extent, std::string &error) override
    {
        if (!renderer_.Resize(extent, error))
            return false;
        Apply();
        return true;
    }
    DeviceStatus Device() override { return renderer_.Device(); }
    ResetResult ResetDevice(std::string &error) override { return renderer_.ResetDevice(error); }
    bool DrawFrame(const kv::CameraView &view, std::string &error) override { return renderer_.DrawFrame(view, error); }
    void Shutdown() noexcept override { renderer_.Shutdown(); }

    float Aspect() const { return aspect_; }
    unsigned Updates() const { return updates_; }

private:
    void Apply()
    {
        aspect_ = vidConfig.aspectRatioWindow;
        ++updates_;
        if (owner_)
            owner_->SetAspect(aspect_);
    }
    EngineSceneRenderer &renderer_;
    kv::FrameOwner *owner_ = nullptr;
    float aspect_ = 0.0f;
    unsigned updates_ = 0;
};

// The second owner the script checks against: the same camera and the same
// input, given to FrameOwner directly as the replay gives it. It draws nothing.
class PoseOnlyRenderer final : public kv::SceneRenderer
{
public:
    bool LoadMap(const std::string &, std::string &) override { return true; }
    void UnloadMap() noexcept override {}
    bool Resize(kv::ViewExtent, std::string &) override { return true; }
    DeviceStatus Device() override { return DeviceStatus::Ready; }
    ResetResult ResetDevice(std::string &) override { return ResetResult::Reset; }
    bool DrawFrame(const kv::CameraView &, std::string &) override { return true; }
    void Shutdown() noexcept override {}
};

// Input events for the adopted window that the script did not push. Counted from
// an SDL event watch, which sees each event as it is queued.
struct StrayInput
{
    uint32_t window = 0;
    std::atomic<unsigned> count{0};
    // Focus taken by the desktop, not by the script: reported, since the input
    // given meanwhile is dropped, as on a real client.
    std::atomic<unsigned> focusLost{0};
};

bool CountStrayInput(void *userdata, SDL_Event *event)
{
    StrayInput &stray = *static_cast<StrayInput *>(userdata);
    uint32_t window = 0, device = 0;
    switch (event->type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        window = event->key.windowID, device = event->key.which;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        window = event->motion.windowID, device = event->motion.which;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        window = event->button.windowID, device = event->button.which;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        window = event->wheel.windowID, device = event->wheel.which;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (event->window.windowID == stray.window && uint32_t(event->window.data1) != kScriptDevice)
            stray.focusLost.fetch_add(1, std::memory_order_relaxed);
        return true;
    default:
        return true;
    }
    if (window == stray.window && device != kScriptDevice)
        stray.count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- the scripted run -----------------------------------------------------------

struct Checkpoint
{
    std::string name;
    kv::ViewExtent extent;
    kv::CameraPose pose, expected;
    bool settled = false, poseMatches = false;
    unsigned draws = 0;
    uint64_t digest = 0;
    uint32_t width = 0, height = 0;
};

struct Step
{
    std::string name;
    std::string result; // "done", or why the window system did not let it happen
    bool exercised = false;
};

class Script
{
public:
    Script(EngineSceneRenderer &renderer, kv::FrameOwner &owner, kv::FrameOwner &reference,
           kv::FreeCameraWindow &window, SDL_Window *sdlWindow)
        : renderer_(renderer), owner_(owner), reference_(reference), window_(window), sdlWindow_(sdlWindow),
          id_(SDL_GetWindowID(sdlWindow))
    {
    }

    // ---- events into the window's own queue ----
    void Push(SDL_Event event, const char *what)
    {
        Require(sdl::PushEvent(event), "script", std::string("SDL did not queue ") + what + ": " + SDL_GetError());
    }
    void PushKey(SDL_Scancode scancode, bool down)
    {
        SDL_Event event = sdl::MakeKey(id_, scancode, down);
        event.key.which = kScriptDevice;
        event.key.down = down;
        Push(event, "a key");
    }
    void PushPress(SDL_Scancode scancode)
    {
        PushKey(scancode, true);
        PushKey(scancode, false);
    }
    void PushClick()
    {
        for (bool down : {true, false})
        {
            SDL_Event event = sdl::MakeButton(id_, SDL_BUTTON_LEFT, down);
            event.button.which = kScriptDevice;
            Push(event, "a mouse button");
        }
    }
    void PushMotion(float dx, float dy)
    {
        SDL_Event event = sdl::MakeMotion(id_, 0.0f, 0.0f, dx, dy);
        event.motion.which = kScriptDevice;
        Push(event, "a mouse motion");
    }
    void PushWindow(sdl::Kind kind, const char *what)
    {
        SDL_Event event = sdl::MakeWindow(id_, kind);
        event.window.data1 = int32_t(kScriptDevice);
        Push(event, what);
    }

    // ---- frames ----
    // One window frame that must draw.
    void Draw(int msec, const std::string &what)
    {
        const std::optional<kv::FrameOutcome> outcome = window_.Tick(0, msec, kSceneTime);
        Require(outcome.has_value(), "script", what + ": the window quit");
        Require(*outcome == kv::FrameOutcome::Drawn, "script",
                what + ": the frame was " + kv::FrameOutcomeName(*outcome) +
                    (owner_.Error().empty() ? std::string() : ": " + owner_.Error()));
        ++frames_;
    }
    // The same frame for the reference owner.
    void Expect(int msec) { reference_.Frame(msec, kSceneTime); }

    // Draws the camera where it is until the view settles, then compares its pose
    // with the reference owner's.
    const Checkpoint &Settle(const std::string &name)
    {
        Checkpoint checkpoint;
        checkpoint.name = name;
        checkpoint.pose = owner_.Camera().Pose();
        checkpoint.expected = reference_.Camera().Pose();
        checkpoint.poseMatches = SamePose(checkpoint.pose, checkpoint.expected);
        std::vector<uint64_t> digests;
        FrameResult last;
        for (int draw = 0; draw < kMaxSettleDraws; ++draw)
        {
            Draw(0, name + " settle draw " + std::to_string(draw));
            last = renderer_.LastFrame();
            digests.push_back(last.stats.digest);
            SayFrame(name.c_str(), draw, last);
            if (wdr::CheckSettled(digests, last.smcPatches).empty())
                break;
        }
        checkpoint.settled = wdr::CheckSettled(digests, last.smcPatches).empty();
        checkpoint.draws = unsigned(digests.size());
        checkpoint.digest = last.stats.digest;
        checkpoint.width = last.stats.width;
        checkpoint.height = last.stats.height;
        checkpoint.extent = owner_.AppliedExtent();
        Require(SamePose(owner_.Camera().Pose(), checkpoint.pose), "script", name + ": settling moved the camera");
        Say("free_camera_window_checkpoint name=%s extent=%s draws=%u settled=%s digest=%s pose_matches=%s %s\n",
            name.c_str(), ExtentText(checkpoint.extent).c_str(), checkpoint.draws, checkpoint.settled ? "yes" : "no",
            Hex64(checkpoint.digest).c_str(), checkpoint.poseMatches ? "yes" : "no", PoseText(checkpoint.pose).c_str());
        if (!checkpoint.poseMatches)
            Say("free_camera_window_expected name=%s %s\n", name.c_str(), PoseText(checkpoint.expected).c_str());
        checkpoints_.push_back(checkpoint);
        return checkpoints_.back();
    }

    const Checkpoint &Find(const std::string &name) const
    {
        for (const Checkpoint &checkpoint : checkpoints_)
            if (checkpoint.name == name)
                return checkpoint;
        Fail("script", "no checkpoint " + name);
    }

    // Frames with no input until `done` holds or the wait runs out. Returns
    // whether it held. Frames here may draw or be skipped as minimized.
    template <typename Done> bool WaitFor(Done done, std::vector<kv::FrameOutcome> *outcomes = nullptr)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWindowWaitMilliseconds);
        while (!done())
        {
            if (std::chrono::steady_clock::now() >= until)
                return false;
            const std::optional<kv::FrameOutcome> outcome = window_.Tick(10, 0, kSceneTime);
            Require(outcome.has_value(), "script", "the window quit while the script waited on it");
            Require(*outcome == kv::FrameOutcome::Drawn || *outcome == kv::FrameOutcome::Minimized, "script",
                    std::string("a frame while waiting on the window was ") + kv::FrameOutcomeName(*outcome) +
                        (owner_.Error().empty() ? std::string() : ": " + owner_.Error()));
            if (outcomes)
                outcomes->push_back(*outcome);
        }
        return true;
    }

    void Record(const std::string &name, bool exercised, const std::string &result)
    {
        steps_.push_back({name, result, exercised});
        Say("free_camera_window_step name=%s exercised=%s result=%s\n", name.c_str(), exercised ? "yes" : "no",
            result.c_str());
    }

    SDL_Window *Window() const { return sdlWindow_; }
    uint32_t Id() const { return id_; }
    kv::FreeCameraWindow &Driver() { return window_; }
    const std::deque<Checkpoint> &Checkpoints() const { return checkpoints_; }
    const std::vector<Step> &Steps() const { return steps_; }
    unsigned Frames() const { return frames_; }

private:
    EngineSceneRenderer &renderer_;
    kv::FrameOwner &owner_, &reference_;
    kv::FreeCameraWindow &window_;
    SDL_Window *sdlWindow_;
    uint32_t id_;
    // A deque: a checkpoint stays where it is while later ones are added.
    std::deque<Checkpoint> checkpoints_;
    std::vector<Step> steps_;
    unsigned frames_ = 0;
};

// The window's logical size, the unit SDL_SetWindowSize takes.
void LogicalSize(SDL_Window *window, int &width, int &height)
{
    Require(SDL_GetWindowSize(window, &width, &height), "script",
            std::string("SDL_GetWindowSize failed: ") + SDL_GetError());
}

bool RelativeModeOn(SDL_Window *window) { return SDL_GetWindowRelativeMouseMode(window); }

// Returns the report's script part, starting with a comma.
std::string RunScript(EngineSceneRenderer &renderer, AspectFollower &follower, kv::FrameOwner &owner,
                       kv::FreeCameraWindow &window, SDL_Window *sdlWindow, const kv::CameraPose &start,
                       const std::string &mapName, std::vector<std::string> &problems)
{
    auto check = [&](const std::string &what, const std::string &problem) {
        if (!problem.empty())
            problems.push_back(what + ": " + problem);
    };
    StrayInput stray;
    stray.window = SDL_GetWindowID(sdlWindow);
    Require(SDL_AddEventWatch(CountStrayInput, &stray), "script",
            std::string("SDL_AddEventWatch failed: ") + SDL_GetError());
    struct Unwatch
    {
        StrayInput &stray;
        ~Unwatch() { SDL_RemoveEventWatch(CountStrayInput, &stray); }
    } unwatch{stray};

    PoseOnlyRenderer poses;
    kv::FrameOwner reference(poses, owner.AppliedExtent());
    Require(reference.Load(mapName, &start), "script", "the reference owner refused the map: " + reference.Error());
    reference.Focus(true);
    reference.SetLookCaptured(true);
    Script script(renderer, owner, reference, window, sdlWindow);

    // 1. Focus and capture through the window. A desktop may not focus a new
    // window on its own, so the script says it is focused, as SDL would.
    script.PushWindow(sdl::Kind::WindowFocusGained, "focus");
    script.Draw(0, "focus");
    Require(owner.Focused() && window.Host().Focused(), "script", "the window did not take focus");
    const kv::ViewExtent base = owner.AppliedExtent();
    Require(!base.Empty(), "script", "the window has no pixel size");
    int logicalWidth = 0, logicalHeight = 0;
    LogicalSize(sdlWindow, logicalWidth, logicalHeight);
    Say("free_camera_window_base extent=%s logical=%dx%d aspect=%.4f\n", ExtentText(base).c_str(), logicalWidth,
        logicalHeight, follower.Aspect());
    script.Settle("W.start");
    script.PushClick();
    script.Draw(0, "the capturing click");
    Require(window.Captured() && RelativeModeOn(sdlWindow), "script",
            "the left click did not capture the pointer" +
                (window.Host().RelativeMouseError().empty() ? std::string()
                                                            : ": " + window.Host().RelativeMouseError()));

    // 2. Path A of the replay, as keys and pointer motion in the window's queue.
    // One second forward.
    script.PushKey(SDL_SCANCODE_W, true);
    reference.Key('w', true);
    for (int i = 0; i < 60; ++i)
    {
        const int msec = 1000 / 60 + (i % 3 == 0);
        script.Draw(msec, "A.forward");
        script.Expect(msec);
    }
    script.PushKey(SDL_SCANCODE_W, false);
    reference.Key('w', false);
    script.Settle("A.forward");
    // A 90 degree turn left: 818 counts at sensitivity 5 and m_yaw 0.022.
    script.PushMotion(-818.0f, 0.0f);
    reference.Mouse(-818, 0);
    script.Draw(0, "A.turn");
    script.Expect(0);
    script.Settle("A.turn");
    // One fast second forward.
    script.PushKey(SDL_SCANCODE_W, true);
    script.PushKey(SDL_SCANCODE_LSHIFT, true);
    reference.Key('w', true);
    reference.Key(K_SHIFT, true);
    for (int i = 0; i < 10; ++i)
    {
        script.Draw(100, "A.fast");
        script.Expect(100);
    }
    script.PushKey(SDL_SCANCODE_W, false);
    script.PushKey(SDL_SCANCODE_LSHIFT, false);
    reference.Key('w', false);
    reference.Key(K_SHIFT, false);
    script.Settle("A.fast");
    // Half a second up.
    script.PushKey(SDL_SCANCODE_SPACE, true);
    reference.Key(K_SPACE, true);
    for (int i = 0; i < 5; ++i)
    {
        script.Draw(100, "A.rise");
        script.Expect(100);
    }
    script.PushKey(SDL_SCANCODE_SPACE, false);
    reference.Key(K_SPACE, false);
    const uint64_t riseDigest = script.Settle("A.rise").digest;

    // 3. A resize by the window itself, to a 16:9 size and back. The engine picks
    // a new aspect for it, and the back buffer must follow the window's pixels.
    {
        const int wideHeight = logicalHeight, wideWidth = (logicalHeight * 16 + 8) / 9;
        const float aspectBefore = follower.Aspect();
        const unsigned resizesBefore = owner.Counters().resizes;
        Require(SDL_SetWindowSize(sdlWindow, wideWidth, wideHeight), "script",
                std::string("SDL_SetWindowSize failed: ") + SDL_GetError());
        SDL_SyncWindow(sdlWindow);
        const bool resized = script.WaitFor([&] {
            const kv::ViewExtent now = window.Extent();
            return !now.Empty() && !(now == base) && owner.AppliedExtent() == now;
        });
        if (!resized)
        {
            script.Record("resize", false, "the window manager kept the window at " + ExtentText(window.Extent()));
        }
        else
        {
            const Checkpoint &wide = script.Settle("A.rise.wide");
            if (wide.width != wide.extent.width || wide.height != wide.extent.height)
                check("resize", "the back buffer is " + std::to_string(wide.width) + "x" +
                                    std::to_string(wide.height) + ", the window " + ExtentText(wide.extent));
            if (follower.Aspect() != vidConfig.aspectRatioWindow)
                check("resize", "the owner's aspect is not the engine's");
            Say("free_camera_window_aspect before=%.4f after=%.4f window=%s\n", aspectBefore, follower.Aspect(),
                ExtentText(wide.extent).c_str());
            Require(SDL_SetWindowSize(sdlWindow, logicalWidth, logicalHeight), "script",
                    std::string("SDL_SetWindowSize failed: ") + SDL_GetError());
            SDL_SyncWindow(sdlWindow);
            const bool back = script.WaitFor([&] { return window.Extent() == base && owner.AppliedExtent() == base; });
            if (!back)
                check("resize", "the window did not return to " + ExtentText(base) + " (it is " +
                                    ExtentText(window.Extent()) + ")");
            else
            {
                const Checkpoint &restored = script.Settle("A.rise.resized");
                if (restored.digest != riseDigest)
                    check("resize", "the settled picture after it (" + Hex64(restored.digest) +
                                        ") is not the one before it (" + Hex64(riseDigest) + ")");
                if (follower.Aspect() != aspectBefore)
                    check("resize", "the aspect did not return with the size");
            }
            script.Record("resize", true,
                          "resizes=" + std::to_string(owner.Counters().resizes - resizesBefore));
        }
    }

    // 4. A minimize by the window itself. Wayland tells a client neither that it
    // was minimized nor lets it restore itself, so there the step is not taken.
    {
        const char *const driver = SDL_GetCurrentVideoDriver();
        if (driver && !std::strcmp(driver, "wayland"))
        {
            script.Record("minimize", false, "Wayland reports no minimized state and cannot restore a window");
        }
        else
        {
            const unsigned drawsBefore = renderer.Counters().draws;
            Require(SDL_MinimizeWindow(sdlWindow), "script", std::string("SDL_MinimizeWindow failed: ") + SDL_GetError());
            SDL_SyncWindow(sdlWindow);
            const bool minimized = script.WaitFor([&] { return window.Host().Minimized(); });
            if (!minimized)
            {
                script.Record("minimize", false, "the window system did not report the window minimized");
                SDL_RestoreWindow(sdlWindow);
                SDL_SyncWindow(sdlWindow);
            }
            else
            {
                const unsigned drawsAtMinimize = renderer.Counters().draws;
                std::vector<kv::FrameOutcome> outcomes;
                for (int i = 0; i < 5; ++i)
                {
                    const std::optional<kv::FrameOutcome> outcome = window.Tick(10, 16, kSceneTime);
                    Require(outcome.has_value(), "script", "the window quit while minimized");
                    outcomes.push_back(*outcome);
                }
                const bool skipped = std::all_of(outcomes.begin(), outcomes.end(), [](kv::FrameOutcome outcome) {
                    return outcome == kv::FrameOutcome::Minimized;
                });
                if (!skipped || renderer.Counters().draws != drawsAtMinimize)
                    check("minimize", "frames were drawn while the window was minimized");
                Require(SDL_RestoreWindow(sdlWindow), "script", std::string("SDL_RestoreWindow failed: ") + SDL_GetError());
                SDL_SyncWindow(sdlWindow);
                const bool restored = script.WaitFor([&] { return !window.Host().Minimized(); });
                if (!restored)
                    Fail("script", "the window system did not restore the window");
                // A restored window is not always given focus back; the script says
                // it has it, as it did at the start.
                if (!window.Host().Focused())
                    script.PushWindow(sdl::Kind::WindowFocusGained, "focus");
                script.Draw(0, "the restored window");
                if (!window.Captured())
                    check("minimize", "capture did not come back with focus");
                const Checkpoint &after = script.Settle("A.rise.restored");
                if (after.digest != riseDigest)
                    check("minimize", "the settled picture after it (" + Hex64(after.digest) +
                                          ") is not the one before it (" + Hex64(riseDigest) + ")");
                script.Record("minimize", true,
                              "skipped_frames=" + std::to_string(outcomes.size()) + " draws_before=" +
                                  std::to_string(drawsAtMinimize - drawsBefore));
            }
        }
    }

    // 5. Focus lost while moving: the held key and the capture go with it, and
    // focus coming back presses nothing again.
    {
        script.PushKey(SDL_SCANCODE_W, true);
        reference.Key('w', true);
        script.Draw(100, "focus.moving");
        script.Expect(100);
        const kv::CameraPose moving = owner.Camera().Pose();
        script.PushWindow(sdl::Kind::WindowFocusLost, "focus loss");
        reference.Focus(false);
        for (int i = 0; i < 3; ++i)
        {
            script.Draw(100, "focus.lost");
            script.Expect(100);
        }
        if (!SamePose(owner.Camera().Pose(), moving))
            check("focus", "the camera moved after focus was lost");
        if (window.Captured() || RelativeModeOn(sdlWindow))
            check("focus", "the pointer stayed captured after focus was lost");
        script.PushWindow(sdl::Kind::WindowFocusGained, "focus");
        reference.Focus(true);
        script.Draw(100, "focus.regained");
        script.Expect(100);
        if (!SamePose(owner.Camera().Pose(), moving))
            check("focus", "the camera moved after focus came back");
        if (!window.Captured() || !RelativeModeOn(sdlWindow))
            check("focus", "the standing capture did not come back with focus");
        // The key is still down on the keyboard as far as the script goes; its
        // release must not move anything either.
        script.PushKey(SDL_SCANCODE_W, false);
        reference.Key('w', false);
        script.Settle("A.focus");
        script.Record("focus", true, "done");
    }

    // 6. Escape releases the pointer, Escape again quits; no frame after that.
    {
        script.PushPress(SDL_SCANCODE_ESCAPE);
        script.Draw(0, "the releasing escape");
        if (window.Captured() || RelativeModeOn(sdlWindow))
            check("escape", "the first Escape did not release the pointer");
        if (!window.Running())
            check("escape", "the first Escape quit");
        script.PushPress(SDL_SCANCODE_ESCAPE);
        const unsigned drawsBefore = renderer.Counters().draws;
        const std::optional<kv::FrameOutcome> outcome = window.Tick(0, 0, kSceneTime);
        if (outcome.has_value())
            check("escape", "the second Escape did not quit");
        if (window.Tick(0, 0, kSceneTime).has_value() || renderer.Counters().draws != drawsBefore)
            check("escape", "a frame ran after the window quit");
        script.Record("escape", true, "done");
    }

    // 7. The rules on what was drawn.
    for (const Checkpoint &checkpoint : script.Checkpoints())
    {
        if (!checkpoint.settled)
            check(checkpoint.name, "did not settle in " + std::to_string(kMaxSettleDraws) + " draws");
        if (!checkpoint.poseMatches)
            check(checkpoint.name, "the camera is at " + PoseText(checkpoint.pose) + ", input given directly puts it at " +
                                       PoseText(checkpoint.expected));
        if (checkpoint.width != checkpoint.extent.width || checkpoint.height != checkpoint.extent.height)
            check(checkpoint.name, "the back buffer is " + std::to_string(checkpoint.width) + "x" +
                                       std::to_string(checkpoint.height) + ", not " + ExtentText(checkpoint.extent));
    }
    check("path A", wdr::CheckViewsDiffer(script.Find("W.start").digest, riseDigest));
    // Input from outside the script is not a fault by itself (a desktop may send
    // pointer motion as the window appears under the pointer, and the owner
    // ignores motion while nothing is captured), but it explains a pose that
    // differs, so it is counted and named next to any problem.
    const unsigned strayCount = stray.count.load();
    if (strayCount && !problems.empty())
        problems.push_back("note: " + std::to_string(strayCount) +
                           " key, pointer or wheel event(s) reached the window from outside the script; the mouse "
                           "or keyboard may have been used during the run");

    // 8. The report's script part.
    std::ostringstream body;
    body << ",\"video_driver\":" << Json(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none")
         << ",\"base\":[" << base.width << "," << base.height << "],\"logical\":[" << logicalWidth << ","
         << logicalHeight << "],\"checkpoints\":[";
    bool first = true;
    for (const Checkpoint &checkpoint : script.Checkpoints())
    {
        body << (first ? "" : ",") << "\n{\"name\":" << Json(checkpoint.name) << ",\"extent\":["
             << checkpoint.extent.width << "," << checkpoint.extent.height << "],\"pose\":" << PoseJson(checkpoint.pose)
             << ",\"pose_matches\":" << (checkpoint.poseMatches ? "true" : "false")
             << ",\"settled\":" << (checkpoint.settled ? "true" : "false") << ",\"draws\":" << checkpoint.draws
             << ",\"digest\":\"" << Hex64(checkpoint.digest) << "\"}";
        first = false;
    }
    body << "],\"steps\":[";
    first = true;
    for (const Step &step : script.Steps())
    {
        body << (first ? "" : ",") << "{\"name\":" << Json(step.name)
             << ",\"exercised\":" << (step.exercised ? "true" : "false") << ",\"result\":" << Json(step.result) << "}";
        first = false;
    }
    body << "],\"script_frames\":" << script.Frames() << ",\"stray_input_events\":" << strayCount
         << ",\"desktop_focus_losses\":" << stray.focusLost.load();
    Say("free_camera_window_script frames=%u stray_input_events=%u desktop_focus_losses=%u\n", script.Frames(),
        strayCount, stray.focusLost.load());
    return body.str();
}

// ---- the live run ---------------------------------------------------------------

// Returns the report's live part, starting with a comma.
std::string RunLive(kv::FrameOwner &owner, kv::FreeCameraWindow &window, int secondsLimit)
{
    const std::string limitText = secondsLimit ? " limit=" + std::to_string(secondsLimit) + "s" : std::string();
    Say("free_camera_window_controls click=capture escape=release escape_again=quit w/a/s/d=move space/c=up/down "
        "shift=fast arrows=turn close=quit%s\n",
        limitText.c_str());
    const auto started = std::chrono::steady_clock::now();
    const auto limit = started + std::chrono::seconds(secondsLimit);
    std::map<kv::FrameOutcome, unsigned> outcomes;
    long long lastMilliseconds = 0;
    std::string ending = "quit";
    kv::FrameOutcome last = kv::FrameOutcome::Drawn;
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        if (secondsLimit && now >= limit && window.Running())
        {
            ending = "time_limit";
            window.RequestQuit();
        }
        // Whole elapsed milliseconds, differenced: the steps add up to the time
        // that passed however fast frames come.
        const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        const int msec = int(std::min<long long>(elapsed - lastMilliseconds, 1000));
        lastMilliseconds = elapsed;
        // Nothing to draw: wait for the next event instead of spinning.
        const bool idle = window.Extent().Empty() || last == kv::FrameOutcome::DeviceLost;
        const int wait = idle ? kIdleWaitMilliseconds : 0;
        const std::optional<kv::FrameOutcome> outcome = window.Tick(wait, msec, int(elapsed & 0x3fffffff));
        if (!outcome)
            break;
        ++outcomes[*outcome];
        last = *outcome;
        if (*outcome == kv::FrameOutcome::Failed || *outcome == kv::FrameOutcome::Stopped)
        {
            ending = kv::FrameOutcomeName(*outcome);
            break;
        }
    }
    const double seconds = Seconds(started);
    const unsigned drawn = outcomes[kv::FrameOutcome::Drawn];
    Say("free_camera_window_live ending=%s seconds=%.2f drawn=%u minimized=%u device_lost=%u fps=%.1f "
        "capture_requests=%u\n",
        ending.c_str(), seconds, drawn, outcomes[kv::FrameOutcome::Minimized], outcomes[kv::FrameOutcome::DeviceLost],
        seconds > 0 ? drawn / seconds : 0.0, window.CaptureRequests());
    std::ostringstream body;
    body << ",\"ending\":" << Json(ending) << ",\"live_seconds\":" << seconds << ",\"outcomes\":{";
    bool first = true;
    for (const auto &[outcome, count] : outcomes)
    {
        body << (first ? "" : ",") << Json(kv::FrameOutcomeName(outcome)) << ":" << count;
        first = false;
    }
    body << "},\"capture_requests\":" << window.CaptureRequests() << ",\"final_pose\":" << PoseJson(owner.Camera().Pose());
    return body.str();
}
} // namespace

void Kisak_RunFreeCameraWindow(const std::filesystem::path &directory, const char *mapName, FreeCameraInput input,
                               int secondsLimit)
{
    const bool scripted = input == FreeCameraInput::Script;
    const double startSeconds = WorldDrawStartSeconds();
    const auto started = std::chrono::steady_clock::now();
    RequireStartedRenderer();
    Say("free_camera_graphics_zones=loaded zones=%d seconds=%.2f\n", g_zoneCount, startSeconds);
    const kv::ViewExtent engineExtent{vidConfig.sceneWidth, vidConfig.sceneHeight};

    // The script reads every frame back to compare settled pictures; a live run
    // only draws.
    EngineSceneRenderer renderer(engineExtent, scripted);
    AspectFollower follower(renderer);
    std::vector<std::string> problems;
    auto check = [&](const std::string &what, const std::string &problem) {
        if (!problem.empty())
            problems.push_back(what + ": " + problem);
    };
    Camera a, b;
    std::string worldName, modeBody;
    kv::FrameCounters owned;
    // Declared before the window: the owner outlives it, and its destructor shuts
    // the renderer down also after a failure.
    kv::FrameOwner owner(follower, engineExtent);
    follower.Attach(owner);
    try
    {
        // 1. The map at its intermission camera (view A of the replay).
        Require(owner.Load(mapName), "map", "the first load: " + owner.Error());
        worldName = renderer.WorldName();
        ChooseCameras(*rgp.world, a, b);
        const kv::CameraPose start = PoseOf(a);
        Require(owner.Load(mapName, &start), "map", "the load at camera A: " + owner.Error());
        Say("free_camera_map=loaded map=%s world=%s camera=%s\n", mapName, worldName.c_str(), a.classname.c_str());

        // 2. The renderer's window, adopted. Destroyed at the end of this block,
        // before the owner's shutdown destroys the window.
        SDL_Window *const sdlWindow = static_cast<SDL_Window *>(renderer.Window());
        Require(sdlWindow != nullptr, "window", "the renderer has no window");
        const SDL_WindowID windowId = SDL_GetWindowID(sdlWindow);
        Require(windowId != 0, "window", std::string("the renderer's window has no SDL id: ") + SDL_GetError());
        {
            kv::FreeCameraWindow window(owner, sdlWindow);
            Say("free_camera_window=adopted id=%u video_driver=%s focused=%s extent=%s\n", unsigned(windowId),
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none",
                window.Host().Focused() ? "yes" : "no", ExtentText(window.Extent()).c_str());
            if (scripted)
                modeBody = RunScript(renderer, follower, owner, window, sdlWindow, start, mapName, problems);
            else
                modeBody = RunLive(owner, window, secondsLimit);
        }
        Require(SDL_GetWindowFromID(windowId) == sdlWindow, "window",
                "the window was gone before the renderer's shutdown (the window host must not destroy it)");

        // 3. The owner's shutdown: the map's unload, then R_Shutdown(1), which
        // destroys the window.
        owner.Shutdown();
        Require(owner.State() == kv::FrameState::Stopped, "shutdown",
                std::string("the owner is ") + kv::FrameStateName(owner.State()) + " after its shutdown");
        owned = owner.Counters();
        check("owner", owner.Error());
        check("renderer", renderer.Problem());
        if (SDL_GetWindowFromID(windowId) != nullptr)
            check("shutdown", "the renderer's shutdown left its window");
        if (!renderer.ShutDown() || dx.device || dx.d3d9 || rg.registered || MapHeld() || g_zoneCount != 0)
            check("shutdown", "the renderer still holds its device, registration, a map or a zone (zones=" +
                                  std::to_string(g_zoneCount) + ")");
        const kisak_engine_view::EngineCounters &engine = renderer.Counters();
        if (owned.draws != engine.draws)
            check("lifecycle", "the owner counted " + std::to_string(owned.draws) + " draws, the renderer " +
                                   std::to_string(engine.draws));
        if (owned.resizes != engine.resizes)
            check("lifecycle", "the owner counted " + std::to_string(owned.resizes) + " resizes, the renderer " +
                                   std::to_string(engine.resizes));

        const double seconds = Seconds(started);
        Say("free_camera_counters loads=%u unloads=%u resizes=%u resets=%u draws=%u empty_frames=%u frames=%u "
            "aspect_updates=%u\n",
            owned.loads, owned.unloads, owned.resizes, owned.resets, owned.draws, engine.emptyFrames, owned.frames,
            follower.Updates());
        Say("free_camera_shutdown=done device=%s zones=%d window=%s\n", dx.device ? "held" : "released", g_zoneCount,
            SDL_GetWindowFromID(windowId) ? "kept" : "destroyed");

        // 4. The report: counts and measurements, no pixels.
        std::ostringstream json;
        json << "{\"schema\":1,\"tool\":" << Json(Tool()) << ",\"input\":\"" << (scripted ? "script" : "window")
             << "\",\"map\":" << Json(mapName) << ",\"world\":" << Json(worldName)
             << ",\"renderer\":\"shader_model_3\",\"engine_window\":[" << engineExtent.width << ","
             << engineExtent.height << "],\"scene_time\":" << (scripted ? "0" : "\"elapsed\"")
             << ",\"camera\":" << CameraJson(a) << modeBody << ",\"owner\":{\"frames\":" << owned.frames
             << ",\"draws\":" << owned.draws << ",\"loads\":" << owned.loads << ",\"unloads\":" << owned.unloads
             << ",\"resizes\":" << owned.resizes << ",\"resets\":" << owned.resets
             << "},\"renderer\":{\"draws\":" << engine.draws << ",\"empty_frames\":" << engine.emptyFrames
             << ",\"resizes\":" << engine.resizes << ",\"resets\":" << engine.resets
             << "},\"aspect_updates\":" << follower.Updates() << ",\"start_seconds\":" << startSeconds
             << ",\"seconds\":" << seconds << ",\"problem_count\":" << problems.size() << ",\"problems\":[";
        for (size_t i = 0; i < problems.size(); ++i)
            json << (i ? "," : "") << "\n" << Json(problems[i]);
        json << "]}\n";
        WriteExclusiveFile(directory / "free-camera-window.json", json.str());

        for (const std::string &problem : problems)
            Say("free_camera_problem %s\n", problem.c_str());
        Say("free_camera_window_run=%s input=%s problems=%zu seconds=%.2f\n", problems.empty() ? "passed" : "failed",
            scripted ? "script" : "window", problems.size(), seconds);
    }
    catch (const std::exception &failure)
    {
        Say("free_camera_failure %s\n", failure.what());
        throw;
    }
    Require(problems.empty(), "check",
            std::to_string(problems.size()) + " window rule(s) failed (see free_camera_problem lines)");
}
