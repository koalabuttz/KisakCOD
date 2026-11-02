#pragma once

// Feeds one engine window's input to the free-camera FrameOwner.
//
// The window is the one the engine renderer created for DXVK (R_CreateWindow
// through src/linux/client/window/win32_window_sdl3.cpp). This class adopts it
// with WindowHost: it pumps that window's SDL events and reads its pixel extent,
// but never presents to it and never destroys it. It is the only event pump in
// the process; the SceneRenderer adapter never reads input.
//
// Rules, all checked by tests/linux_free_camera/free_camera_window_test.cpp:
//   * keys reach FrameOwner::Key as engine key codes (WindowHost's physical US
//     positions), pointer counts reach FrameOwner::Mouse;
//   * look follows the window's actual relative capture: a left click in the
//     focused window asks for capture, Escape releases it, and Escape without
//     capture asks to quit. Those three presses are not passed on;
//   * focus loss releases held keys and capture (WindowHost) and the owner's
//     input; focus gain does not re-press anything;
//   * the extent passed on is the window's pixel size, or empty while the window
//     is minimized, so the owner skips draws instead of resizing to nothing;
//   * quitting only stops the loop. Shutting the renderer down stays with the
//     caller, after this object is destroyed, because the renderer may destroy
//     the window this object borrows.
//
// Single thread, like FrameOwner and WindowHost.

#include "frame_owner.h"

#include <linux/client/window/window_host.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace kisak::view {

struct FreeCameraWindowOptions {
    // Ask for relative capture as soon as the window has focus.
    bool captureOnStart = false;
};

class FreeCameraWindow {
public:
    // `window` is borrowed and must outlive this object; `owner` too.
    FreeCameraWindow(FrameOwner &owner, SDL_Window *window, const FreeCameraWindowOptions &options = {});
    ~FreeCameraWindow();
    FreeCameraWindow(const FreeCameraWindow &) = delete;
    FreeCameraWindow &operator=(const FreeCameraWindow &) = delete;

    // Pump the window's events (waiting up to waitMilliseconds for the first),
    // pass the extent and capture state on, then run one owner frame. Returns
    // no outcome, and runs no frame, once the window has asked to quit.
    std::optional<FrameOutcome> Tick(int waitMilliseconds, int msec, int timeMilliseconds);
    // Events only, for a caller that times frames itself.
    bool Pump(int waitMilliseconds = 0);

    bool Running() const { return host_.Running(); }
    void RequestQuit() { host_.RequestQuit(); }
    // Relative capture as the window has it, which is what look follows.
    bool Captured() const { return host_.RelativeMouseActive(); }
    // The extent the owner is given: pixels, or empty while minimized.
    ViewExtent Extent() const;
    const window::WindowHost &Host() const { return host_; }
    window::WindowHost &Host() { return host_; }
    unsigned CaptureRequests() const { return captureRequests_; }

private:
    class Sink final : public window::EventSink {
    public:
        explicit Sink(FreeCameraWindow &window) : window_(window) {}
        uint32_t NowMilliseconds() const override;
        void Key(uint32_t time, int key, bool down) override;
        void Text(uint32_t, std::string_view) override {}
        void Mouse(uint32_t time, int x, int y, int dx, int dy) override;
        // The owner is given the pixel extent each tick instead of the logical
        // size this reports.
        void Resize(int, int) override {}
        void Focus(bool focused) override;
        void Quit() override {}

    private:
        FreeCameraWindow &window_;
    };

    void OnKey(int key, bool down);
    void Capture(bool on);
    void SyncLook();

    FrameOwner &owner_;
    Sink sink_;
    // Declared after the sink: destroyed first, while the sink still exists to
    // receive the releases and quit the host sends on the way out.
    window::WindowHost host_;
    bool closing_ = false;
    unsigned captureRequests_ = 0;
};

} // namespace kisak::view
