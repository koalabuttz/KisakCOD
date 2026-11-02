// tests/linux_free_camera/frame_owner_test.cpp
//
// Asset-free lifecycle tests for the free-camera frame owner. The renderer is a
// recording fake: every call is logged and checked against the order the owner
// promises (frame_owner.h). Nothing here creates a window or a device, loads a
// map, or draws a pixel.
//
// Coverage: resize coalescing and minimize, device loss with retried and failed
// resets, reload/load/unload, shutdown order (also deferred from inside a draw and
// repeated from the destructor), fail-closed draw/resize/exception paths, focus
// and pointer-capture input rules, two scripted reference traversals with exact
// expected poses, and a seeded randomized run whose invariants are checked inside
// the fake renderer on every call.
//
// What is not claimed: that a real renderer honors SceneRenderer, that D3D9 reset
// works on any device, or that any map is drawn. Those need the static map view and
// hardware evidence.

#include "check.h"

#include <linux/client/view/frame_owner.h>
#include <ui/keycodes.h>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace kisak::view;
using namespace free_camera_check;

namespace {

std::string ExtentText(ViewExtent e) { return std::to_string(e.width) + "x" + std::to_string(e.height); }

class RecordingRenderer : public SceneRenderer {
public:
    explicit RecordingRenderer(ViewExtent extent) : extent_(extent) {}

    std::vector<std::string> log;
    // Scripted answers.
    std::vector<DeviceStatus> deviceAnswers; // consumed front-first; empty means Ready
    std::vector<ResetResult> resetAnswers;   // consumed front-first; empty means Reset
    bool failLoad = false, failResize = false, failDraw = false, throwDraw = false;
    std::function<void()> duringDraw;

    // Invariant bookkeeping, checked on every call.
    bool loaded = false, shutdown = false, lost = false, failedSeen = false;
    int shutdownCalls = 0, violations = 0;
    ViewExtent extent_;
    CameraView lastView;
    std::string lastMap;

    void Violation(const char *what)
    {
        ++violations;
        std::printf("VIOLATION  %s\n", what);
    }
    void Common(const char *call)
    {
        if (shutdown)
            Violation((std::string(call) + " after shutdown").c_str());
    }
    bool LoadMap(const std::string &map, std::string &error) override
    {
        Common("load");
        log.push_back("load:" + map);
        if (loaded) Violation("load while a map is loaded");
        if (failedSeen) Violation("load after a failure");
        if (failLoad) { error = "scripted load failure"; return false; }
        loaded = true;
        lastMap = map;
        return true;
    }
    void UnloadMap() noexcept override
    {
        Common("unload");
        log.push_back("unload");
        if (!loaded) Violation("unload without a map");
        loaded = false;
    }
    bool Resize(ViewExtent extent, std::string &error) override
    {
        Common("resize");
        log.push_back("resize:" + ExtentText(extent));
        if (extent.Empty()) Violation("resize to an empty extent");
        if (lost) Violation("resize while the device is lost");
        if (failedSeen) Violation("resize after a failure");
        if (failResize) { error = "scripted resize failure"; failedSeen = true; return false; }
        extent_ = extent;
        return true;
    }
    DeviceStatus Device() override
    {
        Common("device");
        if (failedSeen) Violation("device query after a failure");
        DeviceStatus status = DeviceStatus::Ready;
        if (!deviceAnswers.empty()) {
            status = deviceAnswers.front();
            deviceAnswers.erase(deviceAnswers.begin());
        }
        lost = status != DeviceStatus::Ready;
        if (status != DeviceStatus::Ready)
            log.push_back(status == DeviceStatus::Lost ? "device:lost" : "device:needs-reset");
        return status;
    }
    ResetResult ResetDevice(std::string &error) override
    {
        Common("reset");
        if (!lost) Violation("reset without a lost device");
        ResetResult result = ResetResult::Reset;
        if (!resetAnswers.empty()) {
            result = resetAnswers.front();
            resetAnswers.erase(resetAnswers.begin());
        }
        log.push_back(result == ResetResult::Reset ? "reset" : result == ResetResult::StillLost ? "reset:still-lost" : "reset:failed");
        if (result == ResetResult::Reset) lost = false;
        if (result == ResetResult::Failed) { error = "scripted reset failure"; failedSeen = true; }
        return result;
    }
    bool DrawFrame(const CameraView &view, std::string &error) override
    {
        Common("draw");
        log.push_back("draw:" + ExtentText({view.width, view.height}));
        if (!loaded) Violation("draw without a map");
        if (lost) Violation("draw while the device is lost");
        if (failedSeen) Violation("draw after a failure");
        if (view.width != extent_.width || view.height != extent_.height) Violation("draw extent differs from the renderer extent");
        if (view.width == 0 || view.height == 0) Violation("draw at an empty extent");
        if (!(view.tanHalfFovX > 0.0f && view.tanHalfFovY > 0.0f)) Violation("draw without a projection");
        lastView = view;
        if (duringDraw) {
            auto callback = duringDraw;
            duringDraw = nullptr;
            callback();
        }
        if (throwDraw) { failedSeen = true; throw std::runtime_error("scripted draw exception"); }
        if (failDraw) { error = "scripted draw failure"; failedSeen = true; return false; }
        return true;
    }
    void Shutdown() noexcept override
    {
        log.push_back("shutdown");
        ++shutdownCalls;
        if (loaded) Violation("shutdown with a map still loaded");
        shutdown = true;
    }
};

bool LogIs(const RecordingRenderer &r, const std::vector<std::string> &expected, const char *label)
{
    const bool same = r.log == expected;
    Check(same, label);
    if (!same) {
        std::printf("      got:");
        for (const auto &entry : r.log) std::printf(" %s", entry.c_str());
        std::printf("\n      expected:");
        for (const auto &entry : expected) std::printf(" %s", entry.c_str());
        std::printf("\n");
    }
    return same;
}

void TestBasicFrame()
{
    RecordingRenderer r({960, 540});
    {
        FrameOwner owner(r, {960, 540});
        Check(owner.Frame(16, 0) == FrameOutcome::NoMap, "no map, no draw");
        Check(owner.State() == FrameState::NoMap, "starts without a map");
        Check(owner.Load("mp_reference"), "load succeeds");
        Check(owner.State() == FrameState::Ready, "ready after load");
        Check(owner.Frame(16, 100) == FrameOutcome::Drawn, "loaded map draws");
        Check(r.lastView.time == 100, "draw receives frame time");
        Check(owner.Counters().draws == 1 && owner.Counters().frames == 2, "counters track frames and draws");
    }
    LogIs(r, {"load:mp_reference", "draw:960x540", "unload", "shutdown"}, "destructor unloads then shuts down");
    Check(r.shutdownCalls == 1 && r.violations == 0, "basic lifecycle has no violations");
}

void TestResize()
{
    RecordingRenderer r({960, 540});
    FrameOwner owner(r, {960, 540});
    owner.Load("mp_reference");
    owner.Resize({800, 600});
    owner.Resize({1024, 768});
    Check(owner.Frame(16, 0) == FrameOutcome::Drawn, "draws after resize");
    owner.Resize({1024, 768});
    owner.Frame(16, 0);
    LogIs(r, {"load:mp_reference", "resize:1024x768", "draw:1024x768", "draw:1024x768"},
          "two resizes coalesce into one before the draw; same extent is not resent");
    Check(owner.AppliedExtent() == ViewExtent{1024, 768}, "applied extent updated");
    Check(owner.Counters().resizes == 1, "one resize counted");

    r.log.clear();
    owner.Resize({0, 0});
    Check(owner.Frame(16, 0) == FrameOutcome::Minimized, "minimized frame is skipped");
    Check(owner.Frame(16, 0) == FrameOutcome::Minimized, "still minimized");
    owner.Resize({1024, 768});
    Check(owner.Frame(16, 0) == FrameOutcome::Drawn, "restore to the old extent draws");
    owner.Resize({0, 768});
    owner.Frame(16, 0);
    owner.Resize({640, 360});
    owner.Frame(16, 0);
    LogIs(r, {"draw:1024x768", "resize:640x360", "draw:640x360"},
          "minimize never resizes the renderer to empty; restore to a new extent resizes once");
    Check(r.violations == 0, "resize sequence has no violations");

    RecordingRenderer start({0, 0});
    {
        FrameOwner minimized(start, {0, 0});
        minimized.Load("mp_reference");
        Check(minimized.Frame(16, 0) == FrameOutcome::Minimized, "a minimized start does not draw");
        minimized.Resize({320, 240});
        Check(minimized.Frame(16, 0) == FrameOutcome::Drawn, "first real extent draws");
    }
    LogIs(start, {"load:mp_reference", "resize:320x240", "draw:320x240", "unload", "shutdown"}, "minimized start lifecycle");
    Check(start.violations == 0, "minimized start has no violations");
}

void TestDeviceLoss()
{
    using DS = SceneRenderer::DeviceStatus;
    using RR = SceneRenderer::ResetResult;
    RecordingRenderer r({960, 540});
    FrameOwner owner(r, {960, 540});
    owner.Load("mp_reference");
    r.deviceAnswers = {DS::Lost, DS::NeedsReset, DS::NeedsReset};
    r.resetAnswers = {RR::StillLost, RR::Reset};
    Check(owner.Frame(16, 0) == FrameOutcome::DeviceLost, "lost device skips the draw");
    Check(owner.State() == FrameState::DeviceLost, "state reports device loss");
    owner.Resize({1280, 720}); // arrives while lost
    Check(owner.Frame(16, 0) == FrameOutcome::DeviceLost, "reset still lost skips the draw");
    Check(owner.Frame(16, 0) == FrameOutcome::Drawn, "successful reset draws again");
    Check(owner.State() == FrameState::Ready, "ready after reset");
    LogIs(r, {"load:mp_reference", "device:lost", "device:needs-reset", "reset:still-lost", "device:needs-reset", "reset",
              "resize:1280x720", "draw:1280x720"},
          "reset precedes the deferred resize, which precedes the draw");
    Check(owner.Counters().resets == 1 && owner.Counters().resetRetries == 1, "reset and retry counted");
    Check(r.violations == 0, "device-loss sequence has no violations");

    RecordingRenderer f({960, 540});
    {
        FrameOwner failing(f, {960, 540});
        failing.Load("mp_reference");
        f.deviceAnswers = {DS::NeedsReset};
        f.resetAnswers = {RR::Failed};
        Check(failing.Frame(16, 0) == FrameOutcome::Failed, "failed reset fails the frame");
        Check(failing.State() == FrameState::Failed, "failed reset is terminal");
        Check(failing.Error() == "scripted reset failure", "reset error is kept");
        Check(failing.Frame(16, 0) == FrameOutcome::Failed, "failed owner keeps refusing");
        Check(!failing.Load("mp_other"), "load is refused after failure");
        Check(!failing.Reload(), "reload is refused after failure");
    }
    LogIs(f, {"load:mp_reference", "device:needs-reset", "reset:failed", "unload", "shutdown"},
          "after a failed reset only unload and shutdown reach the renderer");
    Check(f.violations == 0, "failed reset has no violations");
}

void TestFailClosed()
{
    {
        RecordingRenderer r({960, 540});
        {
            FrameOwner owner(r, {960, 540});
            owner.Load("mp_reference");
            r.failDraw = true;
            Check(owner.Frame(16, 0) == FrameOutcome::Failed, "failed draw fails the frame");
            Check(owner.Error() == "scripted draw failure", "draw error is kept");
            owner.Resize({100, 100});
            owner.Frame(16, 0);
        }
        LogIs(r, {"load:mp_reference", "draw:960x540", "unload", "shutdown"}, "nothing reaches the renderer after a failed draw");
        Check(r.violations == 0, "failed draw has no violations");
    }
    {
        RecordingRenderer r({960, 540});
        {
            FrameOwner owner(r, {960, 540});
            owner.Load("mp_reference");
            r.failResize = true;
            owner.Resize({100, 100});
            Check(owner.Frame(16, 0) == FrameOutcome::Failed, "failed resize fails the frame");
            Check(owner.AppliedExtent() == ViewExtent{960, 540}, "failed resize keeps the old extent");
        }
        LogIs(r, {"load:mp_reference", "resize:100x100", "unload", "shutdown"}, "no draw after a failed resize");
        Check(r.violations == 0, "failed resize has no violations");
    }
    {
        RecordingRenderer r({960, 540});
        {
            FrameOwner owner(r, {960, 540});
            owner.Load("mp_reference");
            r.throwDraw = true;
            Check(owner.Frame(16, 0) == FrameOutcome::Failed, "renderer exception fails the frame");
            Check(owner.Error() == "scripted draw exception", "exception text is kept");
        }
        Check(r.shutdownCalls == 1 && r.violations == 0, "exception path still shuts down once");
    }
    {
        RecordingRenderer r({960, 540});
        FrameOwner owner(r, {960, 540});
        owner.Load("mp_reference");
        r.duringDraw = [&] { owner.Load("mp_other"); };
        Check(owner.Frame(16, 0) == FrameOutcome::Failed, "re-entrant load from a draw fails the frame");
        Check(owner.Error().find("inside a frame") != std::string::npos, "re-entrancy is named in the error");
        owner.Shutdown();
        Check(r.violations == 0, "re-entrant load has no violations");
    }
}

void TestLoadReload()
{
    RecordingRenderer r({960, 540});
    FrameOwner owner(r, {960, 540});
    r.failLoad = true;
    Check(!owner.Load("mp_missing"), "load failure is reported");
    Check(owner.Error() == "scripted load failure", "load error is kept");
    Check(owner.State() == FrameState::NoMap && !owner.MapLoaded(), "load failure leaves no map");
    Check(owner.Frame(16, 0) == FrameOutcome::NoMap, "no draw after a failed load");
    r.failLoad = false;
    CameraPose start;
    start.origin[0] = 100.0f;
    start.angles[1] = 90.0f;
    Check(owner.Load("mp_reference", &start), "a later load succeeds");
    Check(owner.Error().empty(), "successful load clears the error");
    owner.Focus(true);
    owner.Key('w', true);
    owner.Frame(200, 0);
    const CameraPose moved = owner.Camera().Pose();
    Near(moved.origin[1], 38.0, 1e-3, "camera moved before reload");
    Check(owner.Reload(), "reload succeeds");
    Check(!owner.Controls().AnyHeld(), "reload releases held keys");
    Near(owner.Camera().Pose().origin[1], moved.origin[1], 0, "reload keeps the pose");
    Near(owner.Camera().Pose().angles[1], 90.0, 0, "reload keeps the angles");
    owner.Frame(16, 0);
    Near(owner.Camera().Pose().origin[1], moved.origin[1], 0, "released keys do not move the camera after reload");
    Check(owner.Load("mp_second"), "switching maps succeeds");
    owner.Unload();
    Check(owner.Frame(16, 0) == FrameOutcome::NoMap, "no draw after unload");
    Check(!owner.Reload(), "reload without a map is refused");
    LogIs(r, {"load:mp_missing", "load:mp_reference", "draw:960x540", "unload", "load:mp_reference", "draw:960x540", "unload",
              "load:mp_second", "unload"},
          "every load follows an unload of the previous map");
    Check(owner.Counters().loads == 3 && owner.Counters().loadFailures == 1 && owner.Counters().unloads == 3, "load counters");
    Throws<std::invalid_argument>([&] { owner.Load(""); }, "an empty map name is refused");
    Check(r.violations == 0, "load/reload sequence has no violations");
}

void TestShutdown()
{
    RecordingRenderer r({960, 540});
    {
        FrameOwner owner(r, {960, 540});
        owner.Load("mp_reference");
        owner.Shutdown();
        owner.Shutdown();
        Check(owner.State() == FrameState::Stopped, "stopped after shutdown");
        Check(owner.Frame(16, 0) == FrameOutcome::Stopped, "frames after shutdown are refused");
        Check(!owner.Load("mp_other"), "load after shutdown is refused");
        owner.Resize({10, 10});
    }
    LogIs(r, {"load:mp_reference", "unload", "shutdown"}, "repeated and destructor shutdown call the renderer once");
    Check(r.shutdownCalls == 1 && r.violations == 0, "shutdown exactly once");

    RecordingRenderer d({960, 540});
    {
        FrameOwner owner(d, {960, 540});
        owner.Load("mp_reference");
        d.duringDraw = [&] { owner.Shutdown(); };
        Check(owner.Frame(16, 0) == FrameOutcome::Stopped, "shutdown requested inside a draw stops after it");
        Check(owner.State() == FrameState::Stopped, "deferred shutdown completed");
    }
    LogIs(d, {"load:mp_reference", "draw:960x540", "unload", "shutdown"}, "deferred shutdown waits for the draw to return");
    Check(d.shutdownCalls == 1 && d.violations == 0, "deferred shutdown exactly once");

    RecordingRenderer n({960, 540});
    {
        FrameOwner owner(n, {960, 540});
    }
    LogIs(n, {"shutdown"}, "an owner that never loaded only shuts down");
}

void TestInputRules()
{
    RecordingRenderer r({960, 540});
    FrameOwner owner(r, {960, 540});
    owner.Load("mp_reference");
    Check(owner.Key('w', true), "bound key consumed while focused");
    owner.Focus(false);
    Check(!owner.Controls().AnyHeld(), "focus loss releases held keys");
    Check(!owner.Key('w', true), "key-down ignored without focus");
    owner.Frame(200, 0);
    Near(owner.Camera().Pose().origin[0], 0.0, 0, "no movement without focus");
    owner.Focus(true);
    owner.Key('w', true);
    owner.Frame(200, 0);
    Near(owner.Camera().Pose().origin[0], 38.0, 1e-3, "movement resumes with focus");
    owner.Focus(false);
    Check(owner.Key('w', false), "key-up still honored without focus");
    owner.Focus(true);

    owner.Mouse(100, 0);
    owner.Frame(0, 0);
    Near(owner.Camera().Pose().angles[1], 0.0, 0, "pointer counts ignored until capture is active");
    owner.SetLookCaptured(true);
    owner.Mouse(100, 0);
    owner.Frame(0, 0);
    Near(owner.Camera().Pose().angles[1], -11.0, 1e-4, "captured pointer turns the view");
    owner.Mouse(100, 0);
    owner.SetLookCaptured(false);
    owner.Frame(0, 0);
    Near(owner.Camera().Pose().angles[1], -11.0, 1e-4, "losing capture drops pending counts");
    owner.SetLookCaptured(true);
    owner.Mouse(100, 0);
    owner.Focus(false);
    owner.Focus(true);
    owner.Frame(0, 0);
    Near(owner.Camera().Pose().angles[1], -11.0, 1e-4, "focus loss drops pending counts");

    owner.Unload();
    owner.Key('w', true);
    owner.Mouse(1000, 0);
    owner.Frame(200, 0);
    owner.Load("mp_reference");
    owner.Frame(0, 0);
    Near(owner.Camera().Pose().origin[0], 38.0, 1e-3, "input during a missing map never moves the camera");
    Near(owner.Camera().Pose().angles[1], -11.0, 1e-4, "look during a missing map is discarded");

    owner.SetAspect(4.0f / 3.0f);
    owner.Frame(0, 0);
    Near(r.lastView.tanHalfFovX / r.lastView.tanHalfFovY, 4.0 / 3.0, 1e-5, "aspectRatioWindow reaches the view");
    Throws<std::invalid_argument>([&] { owner.SetAspect(NAN); }, "NaN aspect is refused");
    Throws<std::invalid_argument>([&] { owner.Frame(-1, 0); }, "negative frame time is refused");

    bool foreign = false;
    std::thread other([&] {
        try { owner.Frame(16, 0); } catch (const std::logic_error &) { foreign = true; }
    });
    other.join();
    Check(foreign, "a frame from a foreign thread is refused");
    Check(r.violations == 0, "input rules have no violations");
}

// Reference traversals: scripted input through the whole owner with exact
// expected poses. These are the paths a hardware run should replay against a map.
void TestReferenceTraversals()
{
    RecordingRenderer r({1280, 720});
    FrameOwner owner(r, {1280, 720});
    CameraPose start;
    owner.Load("mp_reference", &start);
    owner.SetLookCaptured(true);
    // Path A: one second forward, turn left 90 with the mouse, one fast second
    // forward, then rise for half a second.
    owner.Key('w', true);
    for (int i = 0; i < 60; ++i) owner.Frame(1000 / 60 + (i % 3 == 0), i * 17);
    owner.Key('w', false);
    Near(owner.Camera().Pose().origin[0], 190.0 * (60 * 16 + 20) / 1000.0, 1e-2, "path A leg 1 distance follows frame time");
    const float leg1 = owner.Camera().Pose().origin[0];
    owner.Mouse(-818, 0);
    owner.Mouse(-0, 0);
    owner.Frame(0, 0);
    Near(owner.Camera().Pose().angles[1], 818 * 0.11, 1e-3, "path A mouse turn");
    owner.Key('w', true);
    owner.Key(K_SHIFT, true);
    for (int i = 0; i < 10; ++i) owner.Frame(100, 0);
    owner.Key('w', false);
    owner.Key(K_SHIFT, false);
    const double yaw = 818 * 0.11 * M_PI / 180.0;
    Near(owner.Camera().Pose().origin[0], leg1 + 760.0 * std::cos(yaw), 1e-2, "path A leg 2 x");
    Near(owner.Camera().Pose().origin[1], 760.0 * std::sin(yaw), 1e-2, "path A leg 2 y");
    owner.Key(K_SPACE, true);
    for (int i = 0; i < 5; ++i) owner.Frame(100, 0);
    owner.Key(K_SPACE, false);
    Near(owner.Camera().Pose().origin[2], 95.0, 1e-3, "path A rise");

    // Path B: arrow-key square. Each side is one second forward, then 642 ms of
    // turning left at cl_yawspeed (89.88 degrees: whole milliseconds cannot make an
    // exact quarter turn). The expected heading and position are computed in
    // closed form from those numbers, so turn error is predicted, not tolerated.
    CameraPose origin;
    owner.Load("mp_reference", &origin);
    const double turn = 0.642 * 140.0;
    double expectX = 0.0, expectY = 0.0;
    for (int side = 0; side < 4; ++side) {
        const double heading = side * turn * M_PI / 180.0;
        expectX += 190.0 * std::cos(heading);
        expectY += 190.0 * std::sin(heading);
        owner.Key('w', true);
        for (int i = 0; i < 5; ++i) owner.Frame(200, 0);
        owner.Key('w', false);
        owner.Key(K_LEFTARROW, true);
        for (int i = 0; i < 3; ++i) owner.Frame(200, 0);
        owner.Frame(42, 0);
        owner.Key(K_LEFTARROW, false);
        double expectedYaw = std::fmod((side + 1) * turn + 180.0, 360.0) - 180.0;
        Near(owner.Camera().Pose().angles[1], expectedYaw, 1e-3, "path B heading after each turn");
    }
    Near(owner.Camera().Pose().origin[0], expectX, 1e-2, "path B final x");
    Near(owner.Camera().Pose().origin[1], expectY, 1e-2, "path B final y");
    Near(owner.Camera().Pose().origin[2], 0.0, 1e-4, "path B stays level");
    Check(r.violations == 0, "reference traversals have no violations");
}

void TestRandomized()
{
    using DS = SceneRenderer::DeviceStatus;
    using RR = SceneRenderer::ResetResult;
    uint32_t seed = 0xC0D4u;
    auto next = [&seed](uint32_t bound) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % bound; };
    int totalViolations = 0, owners = 0, draws = 0, failures = 0, shutdowns = 0;
    const int keys[] = {'w', 's', 'a', 'd', 'c', K_SPACE, K_SHIFT, K_LEFTARROW, K_RIGHTARROW, K_UPARROW, K_DOWNARROW, 'x'};
    for (int round = 0; round < 40; ++round) {
        RecordingRenderer r({640, 480});
        {
            FrameOwner owner(r, {640, 480});
            ++owners;
            for (int op = 0; op < 500; ++op) {
                switch (next(14)) {
                case 0: owner.Load(next(2) ? "mp_a" : "mp_b"); break;
                case 1: owner.Reload(); break;
                case 2: if (next(4) == 0) owner.Unload(); break;
                case 3: owner.Resize({next(3) ? 200 + next(1000) : 0, next(5) ? 200 + next(800) : 0}); break;
                case 4: owner.Focus(next(2)); break;
                case 5: owner.SetLookCaptured(next(2)); break;
                case 6: owner.Mouse(int(next(400)) - 200, int(next(400)) - 200); break;
                case 7: case 8: owner.Key(keys[next(12)], next(2)); break;
                case 9: r.deviceAnswers.push_back(next(2) ? DS::Lost : DS::NeedsReset);
                        r.resetAnswers.push_back(next(50) == 0 ? RR::Failed : next(3) ? RR::Reset : RR::StillLost); break;
                case 10: if (next(200) == 0) r.failDraw = true; break;
                case 11: if (next(300) == 0) owner.Shutdown(); break;
                default: {
                    const FrameOutcome outcome = owner.Frame(next(60), op);
                    if (outcome == FrameOutcome::Drawn) ++draws;
                    const CameraPose &pose = owner.Camera().Pose();
                    if (!(pose.angles[0] >= -85.0f && pose.angles[0] <= 85.0f)) { ++totalViolations; std::printf("VIOLATION pitch\n"); }
                    if (!(pose.angles[1] >= -180.0f && pose.angles[1] < 180.0f)) { ++totalViolations; std::printf("VIOLATION yaw\n"); }
                    for (float o : pose.origin)
                        if (!std::isfinite(o)) { ++totalViolations; std::printf("VIOLATION origin\n"); }
                }
                }
            }
            if (owner.State() == FrameState::Failed) ++failures;
            if (owner.State() == FrameState::Stopped) ++shutdowns;
        }
        totalViolations += r.violations;
        if (r.shutdownCalls != 1) { ++totalViolations; std::printf("VIOLATION shutdown count %d\n", r.shutdownCalls); }
        if (r.loaded) { ++totalViolations; std::printf("VIOLATION map left loaded\n"); }
    }
    Check(totalViolations == 0, "randomized lifecycle keeps every renderer invariant");
    Check(draws > 400, "randomized run draws often enough to mean something");
    Check(failures > 0 && shutdowns > 0, "randomized run reaches both failure and early shutdown");
    std::printf("randomized: %d owners, %d draws, %d ended failed, %d shut down early\n", owners, draws, failures, shutdowns);
}

} // namespace

int main()
{
    TestBasicFrame();
    TestResize();
    TestDeviceLoss();
    TestFailClosed();
    TestLoadReload();
    TestShutdown();
    TestInputRules();
    TestReferenceTraversals();
    TestRandomized();
    return Finish("frame_owner_test");
}
