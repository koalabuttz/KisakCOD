#pragma once

#include <SDL.h>
#include <array>
#include <cstdint>
#include <string_view>
#include <string>

namespace kisak::window {
// One WindowHost owns the SDL event queue per process; foreign IDs are ignored.
// The sink must outlive the host and callbacks must not throw.
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
};

struct Config {
    const char *title = "KisakCOD Linux window host";
    int width = 960, height = 540;
    bool hidden = false;
};

class WindowHost {
public:
    WindowHost(EventSink &sink, const Config &config = {});
    ~WindowHost();
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
    uint32_t Id() const { return SDL_GetWindowID(window_); }
    bool Running() const { return running_; }
    bool Focused() const { return focused_; }
    SDL_Window *NativeWindow() const { return window_; }
private:
    void Dispatch(const SDL_Event &event);
    void ReleaseHeld(uint32_t time);
    void SetKey(uint32_t time, int key, bool down, bool repeat = false);
    void CheckThread() const;
    bool ApplyRelativeMouse(bool active);
    uint32_t Time(uint32_t sdlTime) const { return sdlTime + clockOffset_; }
    EventSink &sink_;
    SDL_Window *window_ = nullptr;
    SDL_threadID owner_;
    uint32_t clockOffset_ = 0;
    bool running_ = true, focused_ = false, startedText_ = false;
    bool relativeRequested_=false,relativeActive_=false;
    std::string relativeError_;
    int width_, height_;
    std::array<bool, SDL_NUM_SCANCODES> physical_{};
    std::array<unsigned char, 256> held_{};
    std::array<bool, 5> mouseHeld_{};
};
}
