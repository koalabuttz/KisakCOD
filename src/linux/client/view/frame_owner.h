#pragma once

// One owner for the free-camera frame: the order in which input, camera, window
// extent, device state, map lifetime and the draw happen, and what is refused.
//
// The renderer behind SceneRenderer is the engine renderer brought up by the
// static map view work; this file deliberately knows nothing about D3D9, DXVK or
// refdef_s beyond the camera fields. Tests drive it with a recording renderer.
//
// Guarantees, all checked by tests/linux_free_camera/frame_owner_test.cpp:
//   * nothing is drawn without a loaded map, at an empty extent, or while the
//     device is lost;
//   * a resize is coalesced and applied at the start of a frame, never during a
//     draw, and never with an empty extent (minimize skips draws instead);
//   * a device that needs a reset is reset before the next resize or draw; a
//     reset that is still lost is retried next frame; a failed reset fails closed;
//   * a failed resize or draw fails closed: no further renderer call except
//     UnloadMap and Shutdown;
//   * Load unloads the previous map first and releases held input; Reload keeps
//     the camera pose;
//   * Shutdown unloads, then shuts the renderer down, exactly once, also from the
//     destructor and when requested from inside a frame;
//   * focus loss releases held keys and pending look before the next step.
//
// Single thread: every call must come from the thread that constructed the owner.

#include "free_camera.h"
#include "free_camera_controls.h"

#include <cstdint>
#include <string>
#include <thread>

namespace kisak::view {

class SceneRenderer {
public:
    enum class DeviceStatus { Ready, Lost, NeedsReset };
    enum class ResetResult { Reset, StillLost, Failed };
    virtual ~SceneRenderer() = default;
    // Create the map's resources. On false nothing of the map is held.
    virtual bool LoadMap(const std::string &map, std::string &error) = 0;
    virtual void UnloadMap() noexcept = 0;
    // New back-buffer extent, never empty. May reset the device internally.
    virtual bool Resize(ViewExtent extent, std::string &error) = 0;
    // TestCooperativeLevel's three answers.
    virtual DeviceStatus Device() = 0;
    virtual ResetResult ResetDevice(std::string &error) = 0;
    // Draw and present one frame from this camera.
    virtual bool DrawFrame(const CameraView &view, std::string &error) = 0;
    virtual void Shutdown() noexcept = 0;
};

enum class FrameState { NoMap, Ready, DeviceLost, Failed, Stopped };
enum class FrameOutcome { Drawn, NoMap, Minimized, DeviceLost, Failed, Stopped };

const char *FrameStateName(FrameState state);
const char *FrameOutcomeName(FrameOutcome outcome);

struct FrameCounters {
    unsigned frames = 0, draws = 0, resizes = 0, resets = 0, resetRetries = 0;
    unsigned loads = 0, loadFailures = 0, unloads = 0;
};

class FrameOwner {
public:
    // The renderer is already initialized at `extent` (which may be empty for a
    // minimized start). The owner takes over its lifecycle, not its memory: it
    // calls Shutdown exactly once and the renderer must outlive the owner.
    FrameOwner(SceneRenderer &renderer, ViewExtent extent, const CameraTuning &tuning = {});
    ~FrameOwner();
    FrameOwner(const FrameOwner &) = delete;
    FrameOwner &operator=(const FrameOwner &) = delete;

    // Unload any current map, then load this one. `start` replaces the camera
    // pose; without it the pose is kept. Refused after failure or shutdown.
    bool Load(const std::string &map, const CameraPose *start = nullptr);
    // Load the current map again with the current pose.
    bool Reload();
    void Unload();

    // Window events. Resize is recorded and applied by the next Frame.
    void Resize(ViewExtent extent);
    void Focus(bool focused);
    // The window reports whether relative pointer capture is actually active.
    void SetLookCaptured(bool captured);
    // Engine key code (SE_KEY). Key-up is always honored so nothing sticks;
    // key-down is ignored without focus. Returns true when a binding consumed it.
    bool Key(int key, bool down);
    // Relative pointer counts; ignored unless focused and captured.
    void Mouse(int dx, int dy);
    // aspectRatioWindow from the video config; <= 0 means width/height.
    void SetAspect(float aspect);

    FrameOutcome Frame(int msec, int timeMilliseconds);
    void Shutdown() noexcept;

    FrameState State() const { return state_; }
    const std::string &Error() const { return error_; }
    const std::string &Map() const { return map_; }
    bool MapLoaded() const { return mapLoaded_; }
    ViewExtent AppliedExtent() const { return applied_; }
    const FreeCamera &Camera() const { return camera_; }
    const FreeCameraControls &Controls() const { return controls_; }
    const FrameCounters &Counters() const { return counters_; }
    bool Focused() const { return focused_; }

private:
    FrameOutcome RunFrame(int msec, int timeMilliseconds);
    void CheckCaller(const char *operation) const;
    void Fail(const std::string &what);
    void UnloadNoexcept() noexcept;
    void FinishShutdown() noexcept;
    SceneRenderer &renderer_;
    FreeCamera camera_;
    FreeCameraControls controls_;
    FrameState state_ = FrameState::NoMap;
    std::string error_, map_;
    ViewExtent applied_, pending_;
    bool pendingResize_ = false;
    bool mapLoaded_ = false, focused_ = true, lookCaptured_ = false;
    bool inFrame_ = false, shutdownRequested_ = false, shutdownDone_ = false;
    float aspect_ = 0.0f;
    FrameCounters counters_;
    std::thread::id thread_;
};

} // namespace kisak::view
