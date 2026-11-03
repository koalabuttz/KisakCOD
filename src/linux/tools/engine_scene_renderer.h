#pragma once

// The free camera's SceneRenderer (src/linux/client/view/frame_owner.h) over the
// engine renderer the static map view brought up: map load and unload, the back
// buffer's extent, the device's state and reset, and one drawn frame per camera
// view. Every device object and every draw comes from the engine's own code.
//
// What it does for each call:
//
//   * LoadMap: the map zone and the cgame's map load (world_draw_engine.h
//     LoadMap). On failure every part already loaded is released again.
//   * UnloadMap: R_ShutdownWorld, Com_ShutdownWorld, CM_Shutdown, then the map
//     zone is freed.
//   * Resize: the window size R_ResetDevice reads (dx.windows[0]) and the video
//     config (R_StoreWindowSettings) take the new extent as both display and
//     scene size, then the device is reset as below. The window itself is the
//     window host's: it is never resized or moved here.
//   * Device: TestCooperativeLevel's answer.
//   * ResetDevice: R_RecoverLostDevice's sequence (r_init.cpp) at the current
//     extent. R_RecoverLostDevice itself cannot be called: R_CanRecoverLostDevice
//     asserts that the device reports itself lost, and DXVK never loses a
//     windowed device.
//   * DrawFrame: a refdef from the camera view (FillRefdef), drawn with
//     world_draw_engine.h DrawRefdef. The view must cover the whole scene.
//     With effects on, the map's own effects (engine_effects.h) start after
//     each load, stop before each unload and are updated in every frame.
//   * Shutdown: the client's renderer shutdown, R_Shutdown(1), which also
//     destroys the window and releases the device.
//
// After R_Init and after every device reset the backend's pre-tessellated index
// buffer is unset until one frame has been presented (docs/linux-world-draw-probe.md,
// "The first frame"), so the constructor, Resize and ResetDevice each present one
// empty frame, and every draw first requires the two buffers to agree.
//
// Failures are refusals: the call returns false (or ResetResult::Failed) with the
// reason, and the frame owner fails closed. An engine error (Com_Error,
// Sys_Error, R_FatalInitError when IDirect3DDevice9::Reset fails) ends the
// process, as it does in the client.
//
// One thread: the one that ran R_Init. Built only into KisakCOD-world-draw-probe.

#include <linux/client/view/frame_owner.h>

#include "engine_effects.h"
#include "world_draw_engine.h"

#include <memory>
#include <string>

struct refdef_s;

namespace kisak_engine_view
{

struct EngineCounters
{
    unsigned loads = 0, unloads = 0, resizes = 0, resets = 0, draws = 0, emptyFrames = 0;
};

class EngineSceneRenderer final : public kisak::view::SceneRenderer
{
public:
    // R_Init has run on this thread at `extent`, its graphics zones are loaded,
    // and no map is. readBack measures the back buffer after every draw
    // (LastFrame); without it a frame is drawn and checked but not read back.
    // effects plays the map's own effects (engine_effects.h); a map whose
    // effects cannot be planned then fails to load. Throws std::runtime_error
    // when the renderer is not in that state.
    EngineSceneRenderer(kisak::view::ViewExtent extent, bool readBack, bool effects = false);
    ~EngineSceneRenderer() override;
    EngineSceneRenderer(const EngineSceneRenderer &) = delete;
    EngineSceneRenderer &operator=(const EngineSceneRenderer &) = delete;

    bool LoadMap(const std::string &map, std::string &error) override;
    void UnloadMap() noexcept override;
    bool Resize(kisak::view::ViewExtent extent, std::string &error) override;
    DeviceStatus Device() override;
    ResetResult ResetDevice(std::string &error) override;
    bool DrawFrame(const kisak::view::CameraView &view, std::string &error) override;
    void Shutdown() noexcept override;

    // The SDL_Window* R_CreateWindow made (dx.windows[0].hwnd), for a window host
    // to adopt. Valid from construction until Shutdown, which destroys it.
    void *Window() const;
    // DXVK never reports a windowed device lost, so a test asks for the reset path:
    // the next Device() answers NeedsReset once.
    void RequestReset() { resetRequested_ = true; }

    const kisak_world_draw_engine::FrameResult &LastFrame() const { return last_; }
    // The map's effects, when this renderer plays them; nullptr otherwise.
    const kisak_world_draw_engine::MapEffects *Effects() const { return effects_.get(); }
    const std::string &WorldName() const { return worldName_; }
    kisak::view::ViewExtent Extent() const { return extent_; }
    const EngineCounters &Counters() const { return counters_; }
    bool MapLoaded() const { return mapLoaded_; }
    bool ShutDown() const { return shutdown_; }
    // Why UnloadMap or Shutdown, which cannot report, did not finish.
    const std::string &Problem() const { return problem_; }

private:
    void ResetAtExtent(kisak::view::ViewExtent extent);
    void RequireState(const char *stage) const;
    kisak::view::ViewExtent extent_;
    bool readBack_;
    bool mapLoaded_ = false, shutdown_ = false, resetRequested_ = false;
    std::unique_ptr<refdef_s> refdef_;
    std::unique_ptr<kisak_world_draw_engine::MapEffects> effects_;
    kisak_world_draw_engine::FrameResult last_;
    std::string worldName_, problem_;
    EngineCounters counters_;
};

} // namespace kisak_engine_view
