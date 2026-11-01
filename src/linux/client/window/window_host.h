#pragma once

#include "sdl_api.h"
#include "pixel_buffer.h"
#include <array>
#include <cstdint>
#include <string_view>
#include <string>

namespace kisak::window {
// One WindowHost owns the SDL event queue per process; foreign IDs are ignored.
// The sink must outlive the host. Dispatch propagates callback failures;
// destruction reports failures and still releases all owned SDL resources.
// SDL dispatch and engine interpretation are separate. Timestamps are translated
// once into the sink's monotonic clock epoch, preserving queued-event timing.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual uint32_t NowMilliseconds() const = 0;
    virtual void Key(uint32_t time, int key, bool down) = 0;
    virtual void Text(uint32_t time, std::string_view utf8) = 0;
    virtual void Mouse(uint32_t time, int x, int y, int dx, int dy) = 0;
    virtual void Resize(int width, int height) = 0;
    virtual void Focus(bool focused) = 0;
    virtual void Quit() = 0;
    // Pointer boundary does not imply keyboard focus or release held keys.
    virtual void PointerPresence(bool) {}
};

struct Config {
    const char *title = "KisakCOD Linux window host";
    int width = 960, height = 540;
    bool hidden = false;
};

class WindowHost {
public:
    WindowHost(EventSink &sink, const Config &config = {});
    ~WindowHost() noexcept;
    WindowHost(const WindowHost &) = delete;
    WindowHost &operator=(const WindowHost &) = delete;
    bool Pump(int waitMilliseconds = 0);
    void RequestQuit();
    // Request persists across focus changes. Success while unfocused records the
    // request; RelativeMouseActive reports actual capture, errors are retained.
    bool SetRelativeMouse(bool requested);
    bool RelativeMouseRequested() const { return relativeRequested_; }
    bool RelativeMouseActive() const { return relativeActive_; }
    const std::string &RelativeMouseError() const { return relativeError_; }
    // SDL window-surface presentation only. No game renderer is initialized.
    void Present(uint8_t red, uint8_t green, uint8_t blue);
    // Pixel extent comes from the current SDL surface, not logical resize events.
    // A view must match this extent. Presentation copies synchronously; no view
    // or SDL surface pointer is retained across resize or another frame.
    PixelExtent SurfaceExtent() const;
    void Present(PixelBufferView frame);
    uint32_t Id() const { return SDL_GetWindowID(window_); }
    bool Running() const { return running_; }
    bool Focused() const { return focused_; }
    bool PointerInside() const { return pointerInside_; }
    // Hide the native cursor only while this running, focused window owns the
    // pointer. Restores the previously observed SDL visibility on release.
    void SetEngineCursor(bool requested);
    // Disconnected host policy: legacy gameplay hide requests are recorded but
    // keep a native cursor visible unless custom rendering or relative capture
    // owns it. This is not the full game cursor policy.
    void NativeCursorRequest(bool show);
    bool NativeCursorRequested() const { return nativeCursorRequested_; }
    bool EngineCursorActive() const { return cursorOwned_; }
    SDL_Window *NativeWindow() const { return window_; }
private:
    void Dispatch(const SDL_Event &event);
    void ReleaseHeld(uint32_t time);
    void SetKey(uint32_t time, int key, bool down, bool repeat = false);
    void CheckThread() const;
    bool ApplyRelativeMouse(bool active);
    void ApplyEngineCursor();
    void Motion(uint32_t time, float x, float y, float dx, float dy);
    void Wheel(uint32_t time, float steps, bool flipped);
    // Sub-pixel motion and wheel fractions carried between samples. SDL2
    // delivers integers, so the fractions stay zero there; SDL3 delivers floats
    // and a slow drag must still accumulate. Any interruption clears them.
    void ResetMotion();
    uint32_t Time(uint32_t sdlTime) const { return sdlTime + clockOffset_; }
    EventSink &sink_;
    SDL_Window *window_ = nullptr;
    sdl::ThreadId owner_;
    uint32_t clockOffset_ = 0;
    bool running_ = true, focused_ = false, startedText_ = false, quitComplete_ = false;
    bool relativeRequested_=false,relativeActive_=false;
    std::string relativeError_;
    bool pointerInside_=false,engineCursorRequested_=false,cursorOwned_=false;
    int priorCursorVisibility_=1,initialCursorVisibility_=1;
    bool nativeCursorTouched_=false,nativeCursorRequested_=true;
    float residualDx_=0,residualDy_=0,residualWheel_=0;
    int width_, height_;
    std::array<bool, sdl::kScancodeCount> physical_{};
    std::array<unsigned char, 256> held_{};
    std::array<bool, 5> mouseHeld_{};
};
}
