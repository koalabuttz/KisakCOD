// tests/linux_free_camera/recording_renderer.h
//
// A SceneRenderer that records every call and flags any call the frame owner
// promised never to make (frame_owner.h). Shared by the owner tests and the
// window input tests, so both judge the owner by the same contract.

#pragma once

#include <linux/client/view/frame_owner.h>

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace free_camera_check {

using kisak::view::CameraView;
using kisak::view::SceneRenderer;
using kisak::view::ViewExtent;

inline std::string ExtentText(ViewExtent e) { return std::to_string(e.width) + "x" + std::to_string(e.height); }

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

} // namespace free_camera_check
