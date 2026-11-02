#include "free_camera_window.h"

#include <ui/keycodes.h>

#include <chrono>
#include <stdexcept>

namespace kisak::view {

uint32_t FreeCameraWindow::Sink::NowMilliseconds() const
{
    // The engine clock is 32-bit milliseconds; keep the low bits of a monotonic
    // clock, as WindowHost's own epoch calibration expects.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void FreeCameraWindow::Sink::Key(uint32_t, int key, bool down)
{
    window_.OnKey(key, down);
}

void FreeCameraWindow::Sink::Mouse(uint32_t, int, int, int dx, int dy)
{
    // The owner keeps these only while focused and captured.
    window_.owner_.Mouse(dx, dy);
}

void FreeCameraWindow::Sink::Focus(bool focused)
{
    // WindowHost has already released held keys and capture on a loss, and
    // re-applied a standing capture request on a gain.
    window_.owner_.Focus(focused);
    window_.SyncLook();
}

FreeCameraWindow::FreeCameraWindow(FrameOwner &owner, SDL_Window *window, const FreeCameraWindowOptions &options)
    : owner_(owner), sink_(*this), host_(sink_, window::AdoptedWindow{window})
{
    owner_.Focus(host_.Focused());
    if (options.captureOnStart)
        Capture(true);
    else
        SyncLook();
    owner_.Resize(Extent());
}

FreeCameraWindow::~FreeCameraWindow()
{
    // No more input will come: drop what the owner holds now. The host is
    // destroyed next; it releases capture and the keys it still holds (those
    // releases reach the owner through the sink) and leaves the window alone.
    closing_ = true;
    owner_.Focus(false);
    owner_.SetLookCaptured(false);
}

ViewExtent FreeCameraWindow::Extent() const
{
    if (host_.Minimized())
        return {};
    const auto pixels = host_.SurfaceExtent();
    if (pixels.width < 0 || pixels.height < 0)
        throw std::runtime_error("Window reported a negative pixel size");
    return {static_cast<uint32_t>(pixels.width), static_cast<uint32_t>(pixels.height)};
}

bool FreeCameraWindow::Pump(int waitMilliseconds)
{
    if (!host_.Running())
        return false;
    return host_.Pump(waitMilliseconds);
}

std::optional<FrameOutcome> FreeCameraWindow::Tick(int waitMilliseconds, int msec, int timeMilliseconds)
{
    if (!Pump(waitMilliseconds))
        return std::nullopt;
    owner_.Resize(Extent());
    SyncLook();
    return owner_.Frame(msec, timeMilliseconds);
}

void FreeCameraWindow::OnKey(int key, bool down)
{
    // While the host is being destroyed it only sends releases, and it must not
    // be called back.
    if (down && !closing_) {
        if (key == K_MOUSE1 && !host_.RelativeMouseRequested()) {
            Capture(true);
            return;
        }
        if (key == K_ESCAPE) {
            if (host_.RelativeMouseRequested())
                Capture(false);
            else
                host_.RequestQuit();
            return;
        }
    }
    owner_.Key(key, down);
}

void FreeCameraWindow::Capture(bool on)
{
    if (on)
        ++captureRequests_;
    // A refusal is kept in the host (RelativeMouseError); look then stays off.
    host_.SetRelativeMouse(on);
    SyncLook();
}

void FreeCameraWindow::SyncLook()
{
    owner_.SetLookCaptured(host_.RelativeMouseActive());
}

} // namespace kisak::view
