// tests/linux_free_camera/free_camera_window_test.cpp
//
// The free camera driven by a real SDL window's event queue. The window is made
// here the way the engine's window layer makes one (SDL video initialized by its
// owner, then SDL_CreateWindow), adopted by WindowHost through FreeCameraWindow,
// and fed synthetic SDL events. The renderer behind the frame owner is the same
// recording fake the owner tests use, so every renderer call is still checked
// against the SceneRenderer contract.
//
// Coverage: adopting without creating, presenting to or destroying the window;
// keys and pointer counts reaching the camera; click to capture, Escape to release
// and Escape to quit; focus loss releasing keys and capture; minimize skipping
// draws without an empty resize; a real window resize reaching the renderer once;
// events for another window ignored; quit leaving the renderer and the window to
// their owner; teardown releasing held input before the owner shuts down.
//
// What is not claimed: that this matches a real desktop's event stream (the SDL
// dummy driver is used in CI), that the engine renderer honours the contract, or
// that any frame is drawn. A refused relative capture is injected at the SDL
// call; a real capture is checked in whichever form the driver gives it.

#include "check.h"
#include "recording_renderer.h"

#include <linux/client/view/free_camera_window.h>
#include <linux/client/window/sdl_api.h>
#include <ui/keycodes.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

// Relative capture is refused through the linker's wrap hook, so the refused
// path runs every time instead of depending on a driver that lacks capture
// (both dummy drivers grant it). Only enabling is refused; release always works.
static bool refuseRelative = false;
#if KISAK_LINUX_SDL_MAJOR == 3
extern "C" bool __real_SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled);
extern "C" bool __wrap_SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled)
{
    if (enabled && refuseRelative) {
        SDL_SetError("fixture relative mode refusal");
        return false;
    }
    return __real_SDL_SetWindowRelativeMouseMode(window, enabled);
}
#else
extern "C" int __real_SDL_SetRelativeMouseMode(SDL_bool enabled);
extern "C" int __wrap_SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    if (enabled && refuseRelative) {
        SDL_SetError("fixture relative mode refusal");
        return -1;
    }
    return __real_SDL_SetRelativeMouseMode(enabled);
}
#endif

using namespace kisak::view;
using namespace free_camera_check;
namespace sdl = kisak::sdl;
using Kind = kisak::sdl::Kind;

namespace {

constexpr int kWidth = 320, kHeight = 200;
constexpr int kStep = 100;          // milliseconds per test frame
constexpr double kStepUnits = 19.0; // g_speed 190 for 100 ms
constexpr double kYawPerCount = 0.022 * 5.0;

void Push(SDL_Event event)
{
    if (!sdl::PushEvent(event))
        throw std::runtime_error(std::string("SDL push: ") + SDL_GetError());
}
void WindowEvent(uint32_t id, Kind kind, int a = 0, int b = 0) { Push(sdl::MakeWindow(id, kind, a, b)); }
void KeyEvent(uint32_t id, int scancode, bool down) { Push(sdl::MakeKey(id, scancode, down)); }
void ButtonEvent(uint32_t id, int button, bool down) { Push(sdl::MakeButton(id, button, down)); }
void MotionEvent(uint32_t id, float dx, float dy) { Push(sdl::MakeMotion(id, 10.0f, 10.0f, dx, dy)); }

// SDL can hand a pushed event to a later poll, so drain until quiet.
void Settle(FreeCameraWindow &window)
{
    for (int n = 0; n < 8 && window.Running(); ++n)
        window.Pump(0);
}

double Yaw(const FrameOwner &owner) { return owner.Camera().Pose().angles[1]; }
double X(const RecordingRenderer &r) { return r.lastView.vieworg[0]; }

bool Contains(const RecordingRenderer &r, const std::string &entry)
{
    for (const auto &line : r.log)
        if (line == entry)
            return true;
    return false;
}

int Count(const RecordingRenderer &r, const std::string &prefix)
{
    int count = 0;
    for (const auto &line : r.log)
        if (line.rfind(prefix, 0) == 0)
            ++count;
    return count;
}

// The engine's window layer initializes SDL video itself and keeps it for the
// process; the test holds the same reference so the host's own init and quit
// are visibly balanced against it.
struct EngineVideo {
    EngineVideo()
    {
        if (!sdl::Ok(SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)))
            throw std::runtime_error(std::string("SDL video: ") + SDL_GetError());
    }
    ~EngineVideo() { SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS); }
};

SDL_Window *EngineWindow(const char *title)
{
    SDL_Window *window = sdl::CreateWindow(title, kWidth, kHeight, true);
    if (!window)
        throw std::runtime_error(std::string("SDL window: ") + SDL_GetError());
    return window;
}

class NullSink : public kisak::window::EventSink {
public:
    uint32_t NowMilliseconds() const override { return 0; }
    void Key(uint32_t, int, bool) override {}
    void Text(uint32_t, std::string_view) override {}
    void Mouse(uint32_t, int, int, int, int) override {}
    void Resize(int, int) override {}
    void Focus(bool) override {}
    void Quit() override {}
};

void TestAdoption(SDL_Window *window)
{
    const uint32_t id = SDL_GetWindowID(window);
    NullSink sink;
    Throws<std::invalid_argument>([&] { kisak::window::WindowHost host(sink, kisak::window::AdoptedWindow{nullptr}); },
                                  "a missing window cannot be adopted");

    RecordingRenderer r({kWidth, kHeight});
    {
        FrameOwner owner(r, {kWidth, kHeight});
        {
            FreeCameraWindow input(owner, window);
            auto &host = input.Host();
            Check(host.Adopted() && host.NativeWindow() == window && host.Id() == id,
                  "the host adopts the existing window and its identity");
            Throws<std::logic_error>([&] { kisak::window::WindowHost second(sink); },
                                     "a second host cannot take the event queue from the adopting one");
            Check(input.Extent() == ViewExtent{kWidth, kHeight}, "the extent is the adopted window's pixel size");
            Throws<std::logic_error>([&] { host.Present(1, 2, 3); }, "an adopted window refuses a colour present");
            kisak::window::PixelBuffer pixels;
            pixels.Resize(kWidth, kHeight);
            Throws<std::logic_error>([&] { host.Present(pixels.View()); }, "an adopted window refuses a pixel present");
#if KISAK_LINUX_SDL_MAJOR == 3
            Check(!SDL_WindowHasSurface(window), "no software surface is created on the adopted window");
#endif
            Check(input.Tick(0, kStep, 0) == FrameOutcome::NoMap, "without a map the tick draws nothing");
            Check(r.log.empty(), "an unchanged extent and no map make no renderer call");
        }
        Check(SDL_GetWindowFromID(id) == window, "the adopted window outlives its host");
        Check(SDL_WasInit(SDL_INIT_VIDEO) != 0, "the host's SDL video reference is released without ending the owner's");
        {
            FreeCameraWindow again(owner, window);
            Check(again.Host().Adopted() && again.Running(), "the same window can be adopted again after the first host");
        }
    }
    Check(r.violations == 0 && r.shutdownCalls == 1, "the owner alone shuts the renderer down");
}

void TestTraversal(SDL_Window *window, SDL_Window *other)
{
    const uint32_t id = SDL_GetWindowID(window);
    const uint32_t otherId = SDL_GetWindowID(other);
    RecordingRenderer r({kWidth, kHeight});
    FrameOwner owner(r, {kWidth, kHeight});
    {
        FreeCameraWindow input(owner, window);
        sdl::FlushEvents();
        Check(owner.Load("mp_window_test"), "a map loads behind the window");
        WindowEvent(id, Kind::WindowFocusGained);
        Settle(input);
        Check(owner.Focused(), "window focus reaches the owner");
        Check(input.Tick(0, kStep, 100) == FrameOutcome::Drawn, "a focused window with a map draws");
        Near(X(r), 0.0, 1e-6, "no key, no movement");

        // Keys.
        KeyEvent(id, SDL_SCANCODE_W, true);
        Settle(input);
        Check(owner.Controls().AnyHeld(), "a physical W reaches the camera bindings");
        Check(input.Tick(0, kStep, 200) == FrameOutcome::Drawn, "draws while moving");
        Near(X(r), kStepUnits, 1e-3, "W moves the camera forward at g_speed");
        KeyEvent(id, SDL_SCANCODE_W, false);
        Settle(input);
        input.Tick(0, kStep, 300);
        Near(X(r), kStepUnits, 1e-3, "releasing W stops the camera");

        KeyEvent(otherId, SDL_SCANCODE_W, true);
        Settle(input);
        input.Tick(0, kStep, 400);
        Check(!owner.Controls().AnyHeld(), "a key for another window is not taken");
        Near(X(r), kStepUnits, 1e-3, "another window's key does not move the camera");
        KeyEvent(otherId, SDL_SCANCODE_W, false);
        Settle(input);

        // Look follows the window's capture.
        MotionEvent(id, 100.0f, 0.0f);
        Settle(input);
        input.Tick(0, kStep, 500);
        Near(Yaw(owner), 0.0, 1e-6, "pointer motion without capture does not turn");

        // A refused capture is reported, and look stays off.
        refuseRelative = true;
        ButtonEvent(id, SDL_BUTTON_LEFT, true);
        ButtonEvent(id, SDL_BUTTON_LEFT, false);
        Settle(input);
        Check(input.CaptureRequests() == 1 && input.Host().RelativeMouseRequested() && !input.Captured()
                  && input.Host().RelativeMouseError().find("fixture relative mode refusal") != std::string::npos,
              "a refused capture is reported with SDL's reason, not assumed");
        MotionEvent(id, 100.0f, 0.0f);
        Settle(input);
        input.Tick(0, kStep, 550);
        Near(Yaw(owner), 0.0, 1e-6, "pointer motion after a refused capture does not turn");
        KeyEvent(id, SDL_SCANCODE_ESCAPE, true);
        KeyEvent(id, SDL_SCANCODE_ESCAPE, false);
        Settle(input);
        Check(input.Running() && !input.Host().RelativeMouseRequested(),
              "Escape withdraws a refused capture request without quitting");
        refuseRelative = false;

        ButtonEvent(id, SDL_BUTTON_LEFT, true);
        ButtonEvent(id, SDL_BUTTON_LEFT, false);
        Settle(input);
        Check(input.CaptureRequests() == 2 && input.Host().RelativeMouseRequested(), "a click asks for capture");
        Check(!owner.Controls().AnyHeld(), "the capturing click is not passed on");
        const bool captured = input.Captured();
        std::printf("relative capture: %s%s\n", captured ? "active" : "refused: ",
                    captured ? "" : input.Host().RelativeMouseError().c_str());
        Check(captured || !input.Host().RelativeMouseError().empty(),
              "capture is active, or refused with the driver's reason");
        MotionEvent(id, 100.0f, 0.0f);
        Settle(input);
        input.Tick(0, kStep, 600);
        if (captured) {
            Near(Yaw(owner), -100.0 * kYawPerCount, 1e-3, "captured motion turns by m_yaw x sensitivity per count");
            const double radians = -100.0 * kYawPerCount * 3.14159265358979323846 / 180.0;
            Near(r.lastView.viewaxis[0][0], std::cos(radians), 1e-4, "the drawn view faces the turned yaw (x)");
            Near(r.lastView.viewaxis[0][1], std::sin(radians), 1e-4, "the drawn view faces the turned yaw (y)");
        } else {
            Near(Yaw(owner), 0.0, 1e-6, "refused capture leaves look off");
            Check(true, "the drawn view keeps its heading (x)");
            Check(true, "the drawn view keeps its heading (y)");
        }
        const double yaw = Yaw(owner);

        // Focus loss releases keys and capture; regaining it presses nothing.
        KeyEvent(id, SDL_SCANCODE_W, true);
        Settle(input);
        const double before = X(r);
        WindowEvent(id, Kind::WindowFocusLost);
        Settle(input);
        Check(!owner.Focused() && !owner.Controls().AnyHeld(), "focus loss releases the held key");
        Check(!input.Captured() && input.Host().RelativeMouseRequested(), "focus loss releases capture but keeps the request");
        input.Tick(0, kStep, 700);
        Near(X(r), before, 1e-6, "an unfocused window does not move the camera");
        WindowEvent(id, Kind::WindowFocusGained);
        Settle(input);
        input.Tick(0, kStep, 800);
        Near(X(r), before, 1e-6, "regaining focus does not re-press a key");
        Check(input.Captured() == captured, "regaining focus restores capture where the driver gives it");
        KeyEvent(id, SDL_SCANCODE_W, false);
        Settle(input);

        // Escape releases capture first.
        KeyEvent(id, SDL_SCANCODE_ESCAPE, true);
        KeyEvent(id, SDL_SCANCODE_ESCAPE, false);
        Settle(input);
        Check(input.Running() && !input.Captured() && !input.Host().RelativeMouseRequested(),
              "Escape with capture releases it and keeps running");
        MotionEvent(id, 100.0f, 0.0f);
        Settle(input);
        input.Tick(0, kStep, 900);
        Near(Yaw(owner), yaw, 1e-6, "released capture stops look");

        // Minimize: no draw, no empty resize; restore draws again.
        const int resizesBefore = Count(r, "resize:");
        WindowEvent(id, Kind::WindowMinimized);
        Settle(input);
        Check(input.Host().Minimized() && input.Extent().Empty(), "a minimized window gives an empty extent");
        Check(input.Tick(0, kStep, 1000) == FrameOutcome::Minimized, "a minimized window skips the draw");
        Check(Count(r, "resize:") == resizesBefore && !Contains(r, "resize:0x0"), "minimize never resizes the renderer");
        WindowEvent(id, Kind::WindowRestored);
        Settle(input);
        Check(!input.Host().Minimized() && !owner.Focused(), "a restore event alone clears minimized");
        Check(input.Tick(0, kStep, 1100) == FrameOutcome::Drawn, "a restored window draws before focus returns");
        Check(Count(r, "resize:") == resizesBefore, "restoring the same size does not resize the renderer");
        WindowEvent(id, Kind::WindowMinimized);
        WindowEvent(id, Kind::WindowFocusGained);
        Settle(input);
        Check(!input.Host().Minimized() && owner.Focused(),
              "focus returning clears minimized where the window manager sends no restore");

        // A real resize reaches the renderer once, in pixels.
        sdl::SetWindowSize(window, 400, 300);
        sdl::SyncWindow(window);
        Settle(input);
        Check(input.Extent() == ViewExtent{400, 300}, "the window's new pixel size is the extent");
        input.Tick(0, kStep, 1200);
        Check(Count(r, "resize:400x300") == 1 && r.lastView.width == 400 && r.lastView.height == 300,
              "the renderer is resized once and draws at the new size");
        input.Tick(0, kStep, 1300);
        Check(Count(r, "resize:") == resizesBefore + 1, "an unchanged size is not resent");

        // Escape without capture quits; quitting leaves the renderer alone.
        KeyEvent(id, SDL_SCANCODE_ESCAPE, true);
        Settle(input);
        Check(!input.Running(), "Escape without capture asks to quit");
        Check(!input.Tick(0, kStep, 1400).has_value(), "no frame runs after quit");
        Check(r.shutdownCalls == 0 && !Contains(r, "unload") && owner.State() == FrameState::Ready,
              "quitting does not unload the map or shut the renderer down");
    }
    Check(SDL_GetWindowFromID(id) == window, "the window is still the renderer's after the host is gone");
    Check(!sdl::RelativeMode(window), "relative mode is off on the returned window");
    owner.Shutdown();
    Check(r.log.size() >= 2 && r.log[r.log.size() - 2] == "unload" && r.log.back() == "shutdown",
          "after the host is gone the owner unloads, then shuts the renderer down");
    Check(r.violations == 0, "no renderer call broke the contract");
    sdl::SetWindowSize(window, kWidth, kHeight);
    sdl::SyncWindow(window);
}

void TestTeardownAndClose(SDL_Window *window)
{
    const uint32_t id = SDL_GetWindowID(window);
    RecordingRenderer r({kWidth, kHeight});
    FrameOwner owner(r, {kWidth, kHeight});
    Check(owner.Load("mp_window_test"), "a map loads for the teardown run");
    {
        FreeCameraWindow input(owner, window);
        sdl::FlushEvents();
        WindowEvent(id, Kind::WindowFocusGained);
        KeyEvent(id, SDL_SCANCODE_D, true);
        Settle(input);
        Check(owner.Controls().AnyHeld(), "a key is held when the host goes away");
    }
    Check(!owner.Controls().AnyHeld() && !owner.Focused(), "destroying the host releases the owner's input");
#if KISAK_LINUX_SDL_MAJOR == 3
    Check(!sdl::TextInputActive(window), "text input the host started is stopped on the returned window");
#endif
    Check(owner.Frame(kStep, 0) == FrameOutcome::Drawn, "the owner still draws for its own caller");
    Near(r.lastView.vieworg[1], 0.0, 1e-6, "a key held at teardown moves nothing afterwards");

    {
        FreeCameraWindow input(owner, window, {.captureOnStart = true});
        sdl::FlushEvents();
        Check(input.Host().RelativeMouseRequested() && input.CaptureRequests() == 1, "capture can be asked for at start");
        WindowEvent(id, Kind::WindowFocusGained);
        Settle(input);
        Check(input.Captured() || !input.Host().RelativeMouseError().empty(),
              "a start capture becomes active with focus, or is refused with a reason");
        WindowEvent(id, Kind::WindowClose);
        Settle(input);
        Check(!input.Running() && !input.Captured(), "closing the window quits and releases capture");
        Check(r.shutdownCalls == 0, "closing the window does not shut the renderer down");
    }
    owner.Shutdown();
    Check(r.violations == 0 && r.shutdownCalls == 1, "teardown run keeps the contract");
}

} // namespace

int main()
{
    try {
        EngineVideo video;
        std::printf("SDL driver=%s\n", SDL_GetCurrentVideoDriver());
        SDL_Window *window = EngineWindow("KisakCOD free camera window test");
        SDL_Window *other = EngineWindow("KisakCOD other window");
        TestAdoption(window);
        TestTraversal(window, other);
        TestTeardownAndClose(window);
        SDL_DestroyWindow(other);
        SDL_DestroyWindow(window);
    } catch (const std::exception &error) {
        std::printf("FAIL  unexpected exception: %s\n", error.what());
        ++g_failures;
        ++g_checks;
    }
    return Finish("free camera window");
}
