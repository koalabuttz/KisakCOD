#pragma once

// Single selection point for the SDL release behind the opt-in Linux window
// host. The build sets KISAK_LINUX_SDL_MAJOR (2 by default, 3 for the SDL3
// prototype); no other file includes an SDL header directly.
//
// SDL3 differs from SDL2 in more than names. Success is a bool, event
// timestamps are whole nanoseconds, pointer and wheel motion are floats, input
// text is a borrowed pointer, and relative capture, cursor visibility and text
// input became per-window state. The helpers below are that translation, so
// window_host.cpp, the shells and the tests state engine intent once instead of
// branching on the release at every call.

#ifndef KISAK_LINUX_SDL_MAJOR
#define KISAK_LINUX_SDL_MAJOR 2
#endif

#if KISAK_LINUX_SDL_MAJOR == 3
#include <SDL3/SDL.h>
#elif KISAK_LINUX_SDL_MAJOR == 2
#include <SDL.h>
#else
#error "KISAK_LINUX_SDL_MAJOR selects the Linux window host's SDL release: 2 or 3"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace kisak::sdl {

// ---------------------------------------------------------------------------
// Result conventions
// ---------------------------------------------------------------------------
#if KISAK_LINUX_SDL_MAJOR == 3
// SDL3 reports failure by returning false.
inline bool Ok(bool result) { return result; }
// Polling succeeds with true.
inline bool Got(bool result) { return result; }
// Pushing succeeds with true.
inline bool Pushed(bool result) { return result; }
#else
// SDL2 reports most failures as a negative integer.
inline bool Ok(int result) { return result >= 0; }
// SDL2 polling succeeds with 1 and reports an empty queue with 0.
inline bool Got(int result) { return result != 0; }
// SDL2 pushing succeeds with exactly 1: 0 is a failure and a negative result is
// an event filter that refused the event, so neither may read as success.
inline bool Pushed(int result) { return result == 1; }
#endif

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------
#if KISAK_LINUX_SDL_MAJOR == 3
// SDL3 event timestamps are nanoseconds. Divide before narrowing so a queued
// event does not lose its position to the low 32 bits of the nanosecond value.
inline uint32_t Milliseconds(uint64_t nanoseconds) { return static_cast<uint32_t>(nanoseconds / UINT64_C(1000000)); }
#else
// SDL2 event timestamps are already milliseconds.
inline uint32_t Milliseconds(uint32_t milliseconds) { return milliseconds; }
#endif

// The engine clock is 32-bit milliseconds. SDL3 returns 64-bit milliseconds, so
// keep the low 32 bits of the same modular clock: engine time arithmetic and the
// epoch calibration stay identical in both builds.
inline uint32_t Ticks()
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return static_cast<uint32_t>(SDL_GetTicks());
#else
    return SDL_GetTicks();
#endif
}

// ---------------------------------------------------------------------------
// Bounded numeric conversion
// ---------------------------------------------------------------------------
// SDL2 delivers integer pointer and wheel values; SDL3 delivers floats. One
// converted sample is bounded here, so an implausible or non-finite value is
// rejected instead of being narrowed into an engine integer.
inline constexpr int kMaxAxis = 32768;
// One wheel event moves at most this many engine steps, as before the port.
inline constexpr int kMaxWheelSteps = 16;

// Nearest integer axis position. False when the sample is not finite or is
// outside the accepted range.
inline bool Position(float value, int &position)
{
    if (!std::isfinite(value) || value < -float(kMaxAxis) || value > float(kMaxAxis)) return false;
    position = static_cast<int>(std::lround(value));
    return true;
}

// Whole steps from a sub-pixel sample, truncating toward zero and carrying the
// fraction in `residual` for the next sample. A non-finite sample is rejected
// and abandons the pending fraction.
inline bool Steps(float &residual, float value, int limit, int &steps)
{
    if (!std::isfinite(value)) { residual = 0; steps = 0; return false; }
    const float bounded = std::clamp(value, -float(kMaxAxis), float(kMaxAxis));
    const float accumulated = residual + bounded;
    if (!std::isfinite(accumulated)) { residual = 0; steps = 0; return false; }
    int whole = static_cast<int>(accumulated);
    if (whole > limit || whole < -limit) {
        // Clamping drops the fraction: a saturated sample must not leave a
        // remainder that keeps stepping after the burst is over.
        steps = std::clamp(whole, -limit, limit);
        residual = 0;
        return true;
    }
    steps = whole;
    residual = accumulated - float(whole);
    return true;
}

// A direction change or an interrupted stream abandons the pending fraction.
inline void ResetResidual(float &residual) { residual = 0; }

// Input text is consumed synchronously, so the accepted size is bounded here
// rather than by SDL2's fixed text buffer.
inline constexpr std::size_t kMaxTextBytes = 256;

// ---------------------------------------------------------------------------
// Threads and key codes
// ---------------------------------------------------------------------------
// SDL2 spells the creating-thread identity SDL_threadID and SDL3 SDL_ThreadID;
// both are an unsigned integer, so the window host compares one portable type.
using ThreadId = uint64_t;

#if KISAK_LINUX_SDL_MAJOR == 3
inline ThreadId CurrentThread() { return static_cast<ThreadId>(SDL_GetCurrentThreadID()); }
inline constexpr int kScancodeCount = SDL_SCANCODE_COUNT;
#else
inline ThreadId CurrentThread() { return static_cast<ThreadId>(SDL_ThreadID()); }
inline constexpr int kScancodeCount = SDL_NUM_SCANCODES;
#endif

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
// SDL2 nests the window event kind inside SDL_WINDOWEVENT; SDL3 gives each kind
// its own top-level type. Decode yields one spelling of both.
enum class Kind : uint8_t {
    None,
    Quit,
    WindowClose,
    WindowFocusGained,
    WindowFocusLost,
    WindowMinimized,
    // Back from minimized: restored or maximized.
    WindowRestored,
    WindowEnter,
    WindowLeave,
    WindowResized,
    KeyDown,
    KeyUp,
    Text,
    MouseMotion,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
};

struct Event {
    Kind kind = Kind::None;
    uint32_t window = 0;
    uint32_t time = 0; // SDL clock, whole milliseconds
    int scancode = -1;
    bool repeat = false;
    bool numLock = false; // key events: Num Lock was on
    // Borrowed input text: valid only while the event is being dispatched.
    const char *text = nullptr;
    std::size_t size = 0; // 0 when absent, unterminated or over the bound
    int width = 0, height = 0;
    int button = 0;
    float x = 0, y = 0, dx = 0, dy = 0;
    float wheel = 0;
    bool flipped = false;
};

inline Event Decode(const SDL_Event &source)
{
    Event event;
    event.time = Milliseconds(source.common.timestamp);
#if KISAK_LINUX_SDL_MAJOR == 3
    switch (source.type) {
    case SDL_EVENT_QUIT: event.kind = Kind::Quit; break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        event.window = source.window.windowID;
        event.width = source.window.data1;
        event.height = source.window.data2;
        switch (source.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: event.kind = Kind::WindowClose; break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: event.kind = Kind::WindowFocusGained; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: event.kind = Kind::WindowFocusLost; break;
        case SDL_EVENT_WINDOW_MINIMIZED: event.kind = Kind::WindowMinimized; break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED: event.kind = Kind::WindowRestored; break;
        case SDL_EVENT_WINDOW_MOUSE_ENTER: event.kind = Kind::WindowEnter; break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE: event.kind = Kind::WindowLeave; break;
        default: event.kind = Kind::WindowResized; break;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        event.kind = source.type == SDL_EVENT_KEY_DOWN ? Kind::KeyDown : Kind::KeyUp;
        event.window = source.key.windowID;
        event.scancode = int(source.key.scancode);
        event.repeat = source.key.repeat;
        event.numLock = (source.key.mod & SDL_KMOD_NUM) != 0;
        break;
    case SDL_EVENT_TEXT_INPUT: {
        event.kind = Kind::Text;
        event.window = source.text.windowID;
        // SDL3 hands out a borrowed pointer that SDL may free once this event
        // has been dispatched, so the size is measured now and the bytes are
        // consumed by the sink before Dispatch returns.
        const char *text = source.text.text;
        if (!text) break;
        std::size_t size = 0;
        while (size <= kMaxTextBytes && text[size] != '\0') ++size;
        // Unterminated within the bound is as unusable as SDL2's full buffer.
        if (size <= kMaxTextBytes) { event.text = text; event.size = size; }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        event.kind = Kind::MouseMotion;
        event.window = source.motion.windowID;
        event.x = source.motion.x; event.y = source.motion.y;
        event.dx = source.motion.xrel; event.dy = source.motion.yrel;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        event.kind = source.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Kind::MouseButtonDown : Kind::MouseButtonUp;
        event.window = source.button.windowID;
        event.button = source.button.button;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        event.kind = Kind::MouseWheel;
        event.window = source.wheel.windowID;
        event.wheel = source.wheel.y;
        event.flipped = source.wheel.direction == SDL_MOUSEWHEEL_FLIPPED;
        break;
    default: break;
    }
#else
    switch (source.type) {
    case SDL_QUIT: event.kind = Kind::Quit; break;
    case SDL_WINDOWEVENT:
        event.window = source.window.windowID;
        event.width = source.window.data1;
        event.height = source.window.data2;
        switch (source.window.event) {
        case SDL_WINDOWEVENT_CLOSE: event.kind = Kind::WindowClose; break;
        case SDL_WINDOWEVENT_FOCUS_GAINED: event.kind = Kind::WindowFocusGained; break;
        case SDL_WINDOWEVENT_FOCUS_LOST: event.kind = Kind::WindowFocusLost; break;
        case SDL_WINDOWEVENT_MINIMIZED: event.kind = Kind::WindowMinimized; break;
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_MAXIMIZED: event.kind = Kind::WindowRestored; break;
        case SDL_WINDOWEVENT_ENTER: event.kind = Kind::WindowEnter; break;
        case SDL_WINDOWEVENT_LEAVE: event.kind = Kind::WindowLeave; break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED: event.kind = Kind::WindowResized; break;
        default: break;
        }
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        event.kind = source.type == SDL_KEYDOWN ? Kind::KeyDown : Kind::KeyUp;
        event.window = source.key.windowID;
        event.scancode = int(source.key.keysym.scancode);
        event.repeat = source.key.repeat != 0;
        event.numLock = (source.key.keysym.mod & KMOD_NUM) != 0;
        break;
    case SDL_TEXTINPUT: {
        event.kind = Kind::Text;
        event.window = source.text.windowID;
        std::size_t size = 0;
        while (size < sizeof source.text.text && source.text.text[size] != '\0') ++size;
        // A buffer with no terminator inside SDL's own array is unusable.
        if (size < sizeof source.text.text) { event.text = source.text.text; event.size = size; }
        break;
    }
    case SDL_MOUSEMOTION:
        event.kind = Kind::MouseMotion;
        event.window = source.motion.windowID;
        event.x = float(source.motion.x); event.y = float(source.motion.y);
        event.dx = float(source.motion.xrel); event.dy = float(source.motion.yrel);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        event.kind = source.type == SDL_MOUSEBUTTONDOWN ? Kind::MouseButtonDown : Kind::MouseButtonUp;
        event.window = source.button.windowID;
        event.button = source.button.button;
        break;
    case SDL_MOUSEWHEEL:
        event.kind = Kind::MouseWheel;
        event.window = source.wheel.windowID;
        event.wheel = float(source.wheel.y);
        event.flipped = source.wheel.direction == SDL_MOUSEWHEEL_FLIPPED;
        break;
    default: break;
    }
#endif
    return event;
}

// ---------------------------------------------------------------------------
// Synthetic events
// ---------------------------------------------------------------------------
// The inverse of Decode, for the queue tests. Each builder fills exactly the
// fields Decode reads, so a test states the event it means instead of the
// release's struct layout. SDL3 event text is borrowed: the caller keeps the
// string alive until the event has been pumped.
inline SDL_Event MakeWindow(uint32_t window, Kind kind, int first = 0, int second = 0)
{
    SDL_Event event{};
#if KISAK_LINUX_SDL_MAJOR == 3
    switch (kind) {
    case Kind::WindowClose: event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED; break;
    case Kind::WindowFocusGained: event.type = SDL_EVENT_WINDOW_FOCUS_GAINED; break;
    case Kind::WindowFocusLost: event.type = SDL_EVENT_WINDOW_FOCUS_LOST; break;
    case Kind::WindowMinimized: event.type = SDL_EVENT_WINDOW_MINIMIZED; break;
    case Kind::WindowRestored: event.type = SDL_EVENT_WINDOW_RESTORED; break;
    case Kind::WindowEnter: event.type = SDL_EVENT_WINDOW_MOUSE_ENTER; break;
    case Kind::WindowLeave: event.type = SDL_EVENT_WINDOW_MOUSE_LEAVE; break;
    default: event.type = SDL_EVENT_WINDOW_RESIZED; break;
    }
    event.window.windowID = window;
    event.window.data1 = first;
    event.window.data2 = second;
#else
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = window;
    switch (kind) {
    case Kind::WindowClose: event.window.event = SDL_WINDOWEVENT_CLOSE; break;
    case Kind::WindowFocusGained: event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED; break;
    case Kind::WindowFocusLost: event.window.event = SDL_WINDOWEVENT_FOCUS_LOST; break;
    case Kind::WindowMinimized: event.window.event = SDL_WINDOWEVENT_MINIMIZED; break;
    case Kind::WindowRestored: event.window.event = SDL_WINDOWEVENT_RESTORED; break;
    case Kind::WindowEnter: event.window.event = SDL_WINDOWEVENT_ENTER; break;
    case Kind::WindowLeave: event.window.event = SDL_WINDOWEVENT_LEAVE; break;
    default: event.window.event = SDL_WINDOWEVENT_RESIZED; break;
    }
    event.window.data1 = first;
    event.window.data2 = second;
#endif
    return event;
}

// A key event at an explicit whole-nanosecond timestamp. SDL3 stores it as
// given; SDL2 only has milliseconds, so the same instant is divided once here.
inline SDL_Event MakeKeyAt(uint32_t window, int scancode, bool down, bool repeat, uint64_t nanoseconds,
                           bool numLock = false)
{
    SDL_Event event{};
#if KISAK_LINUX_SDL_MAJOR == 3
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = window;
    event.key.scancode = SDL_Scancode(scancode);
    event.key.repeat = repeat;
    event.key.timestamp = nanoseconds;
    event.key.mod = numLock ? SDL_KMOD_NUM : SDL_KMOD_NONE;
#else
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = window;
    event.key.keysym.scancode = SDL_Scancode(scancode);
    event.key.repeat = repeat ? 1 : 0;
    event.key.timestamp = static_cast<uint32_t>(nanoseconds / UINT64_C(1000000));
    event.key.keysym.mod = numLock ? KMOD_NUM : KMOD_NONE;
#endif
    return event;
}

inline SDL_Event MakeKey(uint32_t window, int scancode, bool down, bool repeat = false, uint32_t milliseconds = 0,
                         bool numLock = false)
{
    return MakeKeyAt(window, scancode, down, repeat, uint64_t(milliseconds) * UINT64_C(1000000), numLock);
}

inline SDL_Event MakeText(uint32_t window, const char *utf8)
{
    SDL_Event event{};
    event.type =
#if KISAK_LINUX_SDL_MAJOR == 3
        SDL_EVENT_TEXT_INPUT;
    // Borrowed, exactly as SDL delivers it.
    event.text.text = utf8;
#else
        SDL_TEXTINPUT;
    // SDL2 stores the text inline and truncates what does not fit.
    std::snprintf(event.text.text, sizeof event.text.text, "%s", utf8);
#endif
    event.text.windowID = window;
    return event;
}

inline SDL_Event MakeQuit()
{
    SDL_Event event{};
#if KISAK_LINUX_SDL_MAJOR == 3
    event.type = SDL_EVENT_QUIT;
#else
    event.type = SDL_QUIT;
#endif
    return event;
}

inline SDL_Event MakeButton(uint32_t window, int button, bool down)
{
    SDL_Event event{};
#if KISAK_LINUX_SDL_MAJOR == 3
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = down;
#else
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
#endif
    event.button.windowID = window;
    event.button.button = Uint8(button);
    return event;
}

inline SDL_Event MakeMotion(uint32_t window, float x, float y, float dx, float dy)
{
    SDL_Event event{};
    event.type =
#if KISAK_LINUX_SDL_MAJOR == 3
        SDL_EVENT_MOUSE_MOTION;
#else
        SDL_MOUSEMOTION;
#endif
    event.motion.windowID = window;
#if KISAK_LINUX_SDL_MAJOR == 3
    event.motion.x = x; event.motion.y = y;
    event.motion.xrel = dx; event.motion.yrel = dy;
#else
    event.motion.x = int(x); event.motion.y = int(y);
    event.motion.xrel = int(dx); event.motion.yrel = int(dy);
#endif
    return event;
}

inline SDL_Event MakeWheel(uint32_t window, float steps, bool flipped = false)
{
    SDL_Event event{};
    event.type =
#if KISAK_LINUX_SDL_MAJOR == 3
        SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = steps;
#else
        SDL_MOUSEWHEEL;
    event.wheel.y = Sint32(steps);
#endif
    event.wheel.windowID = window;
    event.wheel.direction = flipped ? SDL_MOUSEWHEEL_FLIPPED : SDL_MOUSEWHEEL_NORMAL;
    return event;
}

inline bool PushEvent(SDL_Event &event) { return Pushed(SDL_PushEvent(&event)); }

inline void FlushEvents()
{
#if KISAK_LINUX_SDL_MAJOR == 3
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
#else
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
#endif
}

// ---------------------------------------------------------------------------
// Window, cursor, text input and relative capture
// ---------------------------------------------------------------------------
inline SDL_Window *CreateWindow(const char *title, int width, int height, bool hidden)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    // SDL3 windows are shown unless hidden is requested, and it takes no
    // position: the window manager places the window.
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0);
    return SDL_CreateWindow(title, width, height, flags);
#else
    const Uint32 flags = SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    return SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
#endif
}

// The window's drawable size in pixels, read from the window itself rather than
// from a software surface, so it is safe on a window another API presents to.
// SDL2 before 2.26 has no pixel query; there the logical size is the answer,
// which is the same on X11 where SDL2 does no display scaling.
inline bool PixelSize(SDL_Window *window, int &width, int &height)
{
    width = height = 0;
#if KISAK_LINUX_SDL_MAJOR == 3
    return Ok(SDL_GetWindowSizeInPixels(window, &width, &height));
#elif SDL_VERSION_ATLEAST(2, 26, 0)
    if (!window) return false;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    return true;
#else
    if (!window) return false;
    SDL_GetWindowSize(window, &width, &height);
    return true;
#endif
}

inline bool WindowMinimizedFlag(SDL_Window *window)
{
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
}

inline bool SetWindowSize(SDL_Window *window, int width, int height)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return Ok(SDL_SetWindowSize(window, width, height));
#else
    SDL_SetWindowSize(window, width, height);
    return window != nullptr;
#endif
}

// SDL3 applies a requested resize asynchronously, so a caller that needs the new
// extent asks for it explicitly. SDL2 has already applied it when the resize call
// returns. The software surface is rebuilt from whatever extent the window has.
inline void SyncWindow(SDL_Window *window)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    (void)SDL_SyncWindow(window);
#else
    (void)window;
#endif
}

// Cursor visibility is a tri-state integer in SDL2 (negative on failure). SDL3
// splits it into a separate query and bool setters; keep SDL2's integers so the
// window host's restore-previous-visibility rule reads the same.
inline int QueryCursor()
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return SDL_CursorVisible() ? 1 : 0;
#else
    return SDL_ShowCursor(SDL_QUERY);
#endif
}

inline int ShowCursor(int state)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    if (state < 0) return QueryCursor();
    return (state ? SDL_ShowCursor() : SDL_HideCursor()) ? state : -1;
#else
    return SDL_ShowCursor(state);
#endif
}

inline bool SetRelative(SDL_Window *window, bool enabled)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return Ok(SDL_SetWindowRelativeMouseMode(window, enabled));
#else
    (void)window;
    return Ok(SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE));
#endif
}

inline bool RelativeMode(SDL_Window *window)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return SDL_GetWindowRelativeMouseMode(window);
#else
    (void)window;
    return SDL_GetRelativeMouseMode() == SDL_TRUE;
#endif
}

inline bool SetGrab(SDL_Window *window, bool grabbed)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return Ok(SDL_SetWindowMouseGrab(window, grabbed));
#else
    SDL_SetWindowGrab(window, grabbed ? SDL_TRUE : SDL_FALSE);
    return true;
#endif
}

inline bool TextInputActive(SDL_Window *window)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return SDL_TextInputActive(window);
#else
    (void)window;
    return SDL_IsTextInputActive() == SDL_TRUE;
#endif
}

inline bool StartTextInput(SDL_Window *window)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return SDL_StartTextInput(window);
#else
    (void)window;
    SDL_StartTextInput();
    return true;
#endif
}

inline void StopTextInput(SDL_Window *window)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    SDL_StopTextInput(window);
#else
    SDL_StopTextInput();
#endif
}

inline void MouseState(float &x, float &y)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    SDL_GetMouseState(&x, &y);
#else
    int sampleX = 0, sampleY = 0;
    SDL_GetMouseState(&sampleX, &sampleY);
    x = float(sampleX); y = float(sampleY);
#endif
}

// ---------------------------------------------------------------------------
// Software window surface
// ---------------------------------------------------------------------------
inline bool MustLock(SDL_Surface *surface) { return SDL_MUSTLOCK(surface); }
inline bool LockSurface(SDL_Surface *surface) { return Ok(SDL_LockSurface(surface)); }
inline void UnlockSurface(SDL_Surface *surface) { SDL_UnlockSurface(surface); }
inline bool UpdateWindowSurface(SDL_Window *window) { return Ok(SDL_UpdateWindowSurface(window)); }

inline bool FillSurface(SDL_Surface *surface, uint8_t red, uint8_t green, uint8_t blue)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return Ok(SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGB(surface, red, green, blue)));
#else
    return Ok(SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, red, green, blue)));
#endif
}

// Copies one RGBA frame into the window surface. SDL2 takes the surface's pixel
// format as a packed integer, SDL3 as an SDL_PixelFormat value.
inline bool ConvertRgbaToSurface(SDL_Surface *surface, int width, int height, const uint8_t *source, int pitch)
{
#if KISAK_LINUX_SDL_MAJOR == 3
    return Ok(SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_RGBA32, source, pitch,
        surface->format, surface->pixels, surface->pitch));
#else
    return Ok(SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_RGBA32, source, pitch,
        surface->format->format, surface->pixels, surface->pitch));
#endif
}

// Reads one surface pixel back as RGB. SDL2 describes a pixel with the surface's
// SDL_PixelFormat; SDL3 needs the separate format details.
inline bool ReadPixel(SDL_Surface *surface, int x, int y, uint8_t &red, uint8_t &green, uint8_t &blue)
{
    if (!surface || !surface->pixels || x < 0 || y < 0 || x >= surface->w || y >= surface->h) return false;
#if KISAK_LINUX_SDL_MAJOR == 3
    return Ok(SDL_ReadSurfacePixel(surface, x, y, &red, &green, &blue, nullptr));
#else
    const bool lock = MustLock(surface);
    if (lock && !LockSurface(surface)) return false;
    const auto *bytes = static_cast<const uint8_t *>(surface->pixels) + y * surface->pitch + x * surface->format->BytesPerPixel;
    uint32_t pixel = 0;
    std::memcpy(&pixel, bytes, surface->format->BytesPerPixel);
    uint8_t alpha = 0;
    SDL_GetRGBA(pixel, surface->format, &red, &green, &blue, &alpha);
    if (lock) UnlockSurface(surface);
    return true;
#endif
}

} // namespace kisak::sdl
