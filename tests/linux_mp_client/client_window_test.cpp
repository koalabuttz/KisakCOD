// Asset-free checks for the multiplayer client's game window:
// src/linux/client/mp_client_window.cpp over the Win32 window layer
// (src/linux/client/window/win32_window_sdl3.cpp) and WindowHost.
//
// It runs on SDL's dummy video driver: no display server, no GPU, no Vulkan.
// That driver cannot make Vulkan windows, so the linker's wrap hook drops the
// Vulkan flag from the window CreateWindowExA asks SDL for; everything else is
// the production path. The engine is a recorder: what reaches it is checked,
// not what the game does with it.
//
// Coverage: the window procedure adopts the window inside CreateWindowExA and
// lets go inside DestroyWindow, before the window goes; a second window is left
// alone; keys reach the engine queue and pointer boundaries are not engine
// events; motion is handed over once per frame, summed; look mode takes and
// releases relative capture and the cursor follows the engine's requests; focus,
// minimize and close reach the engine as VID_AppActivate and WM_CLOSE do;
// destroying the window (vid_restart, shutdown) neither quits nor clears keys;
// another thread's pump gets nothing, as with Windows' per-thread queues;
// a recreated window is adopted again.
//
// Not claimed: a real desktop's event stream, HiDPI pixel scaling (the dummy
// driver has a pixel density of 1), or anything about a drawn frame.
#include <linux/d3d9_types.h>

#include <linux/client/mp_client_window.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/window/win32_window_sdl3.h>
#include <linux/sys_events.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <ui/keycodes.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" SDL_Window *__real_SDL_CreateWindowWithProperties(SDL_PropertiesID props);
extern "C" SDL_Window *__wrap_SDL_CreateWindowWithProperties(SDL_PropertiesID props)
{
    const Sint64 flags = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, 0);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags & ~Sint64(SDL_WINDOW_VULKAN));
    return __real_SDL_CreateWindowWithProperties(props);
}

namespace {

namespace sdl = kisak::sdl;
using kisak::mp_client::ClientWindow;
using kisak::mp_client::ClientWindowEngine;
using kisak::mp_client::GameWindow;
using Kind = sdl::Kind;

int checks = 0, failures = 0;
void Check(bool condition, const char *what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

struct MouseCall { int x, y, dx, dy; };
struct Activation { bool active, minimized; };
struct Recorder {
    int lookAnswer = 0;
    std::vector<MouseCall> mouse;
    int clearKeys = 0;
    std::vector<Activation> activations;
    int closes = 0;
    std::vector<void *> windows;
    bool windowKnownWhenCreated = false;
} rec;

int Mouse(int x, int y, int dx, int dy)
{
    rec.mouse.push_back({x, y, dx, dy});
    return rec.lookAnswer;
}
void ClearKeys(int) { ++rec.clearKeys; }
void Activate(bool active, bool minimized) { rec.activations.push_back({active, minimized}); }
void Close() { ++rec.closes; }
void WindowChanged(void *hwnd)
{
    if (hwnd)
        rec.windowKnownWhenCreated = IsWindow(static_cast<HWND>(hwnd)) == TRUE;
    rec.windows.push_back(hwnd);
}

ClientWindowEngine Engine()
{
    ClientWindowEngine engine;
    engine.mouse = Mouse;
    engine.clearKeys = ClearKeys;
    engine.activate = Activate;
    engine.close = Close;
    engine.windowChanged = WindowChanged;
    return engine;
}

HWND MakeWindow()
{
    return CreateWindowExA(0, "CoD4", "Call of Duty 4", WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 0, 0, 320, 200, nullptr,
                           nullptr, GetModuleHandleA(nullptr), nullptr);
}

uint32_t IdOf(HWND hwnd) { return SDL_GetWindowID(reinterpret_cast<SDL_Window *>(hwnd)); }

void Push(SDL_Event event)
{
    if (!sdl::PushEvent(event))
        throw std::runtime_error(std::string("SDL push: ") + SDL_GetError());
}

struct Queued { int type, value, value2; };
std::vector<Queued> Drain()
{
    std::vector<Queued> events;
    sysEvent_t event{};
    while (Sys_PollQueuedEvent(&event))
        events.push_back({event.evType, event.evValue, event.evValue2});
    return events;
}

bool HasKey(const std::vector<Queued> &events, int key, int down)
{
    for (const Queued &event : events)
        if (event.type == SE_KEY && event.value == key && event.value2 == down)
            return true;
    return false;
}

int CountType(const std::vector<Queued> &events, int type)
{
    int count = 0;
    for (const Queued &event : events)
        count += event.type == type;
    return count;
}

void CheckCallbacksRequired()
{
    ClientWindowEngine missing = Engine();
    missing.close = nullptr;
    bool refused = false;
    try { kisak::mp_client::InstallWindowProcedure(missing); }
    catch (const std::invalid_argument &) { refused = true; }
    Check(refused, "the window procedure needs every engine callback");
}

void CheckWindowLifetime()
{
    kisak::mp_client::InstallWindowProcedure(Engine());
    Check(GameWindow() == nullptr, "no game window before the renderer makes one");

    const HWND game = MakeWindow();
    Check(game != nullptr, "CreateWindowExA makes the game window on the dummy driver");
    if (!game)
        return;
    Check(rec.windows.size() == 1 && rec.windows[0] == game, "WM_CREATE hands the engine the new window once");
    Check(rec.windowKnownWhenCreated, "the window exists when the engine hears of it");
    Check(GameWindow() && GameWindow()->Host().Adopted(), "the game window is adopted, not created, by its host");

    const HWND other = MakeWindow();
    Check(other != nullptr && GameWindow() && GameWindow()->Host().Id() == IdOf(game),
          "a second window is not adopted while the game window exists");
    Check(rec.windows.size() == 1, "a second window does not replace g_wv.hWnd");
    Check(DestroyWindow(other) == TRUE, "the second window is destroyed");
    Check(GameWindow() && rec.windows.size() == 1, "destroying the second window leaves the game window alone");

    // --- events --------------------------------------------------------------
    // Showing windows makes the driver send its own focus and pointer events.
    // Take them, then start from an unfocused window with the pointer outside.
    const uint32_t id = IdOf(game);
    GameWindow()->Pump();
    Push(sdl::MakeWindow(id, Kind::WindowFocusLost));
    Push(sdl::MakeWindow(id, Kind::WindowLeave));
    GameWindow()->Pump();
    Drain();
    rec.activations.clear();

    Push(sdl::MakeWindow(id, Kind::WindowFocusGained));
    Push(sdl::MakeWindow(id, Kind::WindowEnter));
    Push(sdl::MakeKey(id, SDL_SCANCODE_A, true));
    Push(sdl::MakeText(id, "a"));
    GameWindow()->Pump();
    Check(rec.activations.size() == 1 && rec.activations[0].active && !rec.activations[0].minimized,
          "focus gained activates the app (VID_AppActivate 1, not minimized)");
    Check(GameWindow()->Foreground(), "the focused game window is the foreground window");
    std::vector<Queued> events = Drain();
    Check(HasKey(events, 'a', 1), "a key press reaches the engine queue as SE_KEY");
    Check(CountType(events, SE_CHAR) == 1, "typed text reaches the engine queue as SE_CHAR");
    Check(CountType(events, SE_POINTER) == 1, "the pointer boundary is queued for the platform to drop");
    Check(CountType(events, SE_MOUSE) == 0, "motion is never queued as an engine event");

    // Another thread (the render thread, through Sys_LoadingKeepAlive) gets
    // none of the window's events, as PeekMessage gives it none on Windows.
    Push(sdl::MakeKey(id, SDL_SCANCODE_A, false));
    bool otherPumped = true, otherThrew = false;
    std::thread renderThread([&] {
        try { otherPumped = GameWindow()->Pump(); }
        catch (...) { otherThrew = true; }
    });
    renderThread.join();
    Check(!otherThrew && !otherPumped, "another thread's pump does nothing and does not fail");
    Check(Drain().empty(), "another thread's pump delivers nothing");
    Check(GameWindow()->Pump(), "the window's own thread pumps it");
    Check(HasKey(Drain(), 'a', 0), "a key release reaches the engine queue");

    // --- mouse ----------------------------------------------------------------
    rec.mouse.clear();
    Push(sdl::MakeMotion(id, 10, 20, 3, 4));
    Push(sdl::MakeMotion(id, 15, 22, 5, 2));
    GameWindow()->Pump();
    Check(rec.mouse.empty(), "motion waits for the frame");
    rec.lookAnswer = 1;
    GameWindow()->Frame(true);
    Check(rec.mouse.size() == 1 && rec.mouse[0].x == 15 && rec.mouse[0].y == 22 && rec.mouse[0].dx == 8 &&
              rec.mouse[0].dy == 6,
          "one CL_MouseEvent per frame: the last position and the summed motion");
    Check(GameWindow()->Look() && GameWindow()->Host().RelativeMouseRequested(),
          "look mode asks the window for relative capture");
    GameWindow()->Frame(true);
    Check(rec.mouse.size() == 2 && rec.mouse[1].dx == 0 && rec.mouse[1].dy == 0,
          "a frame without motion still reports, with no motion (as IN_MouseMove does)");

    GameWindow()->ShowSystemCursor(false);
    Check(GameWindow()->CursorHidden() && !GameWindow()->Host().EngineCursorActive(),
          "a cursor request in look mode is recorded, relative capture keeps the cursor");
    rec.lookAnswer = 0;
    GameWindow()->Frame(true);
    Check(!GameWindow()->Look() && !GameWindow()->Host().RelativeMouseRequested(),
          "a menu taking the mouse releases relative capture");
    Check(GameWindow()->Host().EngineCursorActive(), "the recorded hide request applies once capture is released");
    GameWindow()->ShowSystemCursor(true);
    Check(!GameWindow()->Host().EngineCursorActive(), "showing the system cursor gives it back");

    rec.lookAnswer = 1;
    GameWindow()->Frame(true);
    Push(sdl::MakeMotion(id, 30, 30, 7, 7));
    GameWindow()->Pump();
    const size_t before = rec.mouse.size();
    GameWindow()->Frame(false);
    Check(rec.mouse.size() == before && !GameWindow()->Look(),
          "an inactive mouse is released and the engine is not asked");
    GameWindow()->Frame(true);
    Check(rec.mouse.size() == before + 1 && rec.mouse.back().dx == 0,
          "motion gathered while the mouse was inactive is dropped");

    bool warped = true;
    try { GameWindow()->SetCursorPos(40, 50); }
    catch (...) { warped = false; }
    Check(warped, "IN_SetCursorPos warps inside the game window");

    // --- focus, minimize, close ----------------------------------------------
    rec.lookAnswer = 1;
    GameWindow()->Frame(true);
    Push(sdl::MakeKey(id, SDL_SCANCODE_W, true));
    Push(sdl::MakeWindow(id, Kind::WindowFocusLost));
    const int clearsBefore = rec.clearKeys;
    GameWindow()->Pump();
    Check(rec.activations.size() == 2 && !rec.activations[1].active && !rec.activations[1].minimized,
          "focus lost deactivates the app");
    Check(rec.clearKeys > clearsBefore, "focus lost clears the engine's held keys");
    Check(!GameWindow()->Foreground(), "an unfocused window is not the foreground window");
    const size_t unfocused = rec.mouse.size();
    GameWindow()->Frame(true);
    Check(rec.mouse.size() == unfocused && !GameWindow()->Look(), "an unfocused window gives no mouse to the engine");
    Drain();

    Push(sdl::MakeWindow(id, Kind::WindowFocusGained));
    Push(sdl::MakeWindow(id, Kind::WindowMinimized));
    GameWindow()->Pump();
    Check(rec.activations.size() == 4 && !rec.activations[3].active && rec.activations[3].minimized,
          "minimizing deactivates the app and says it is minimized");
    Drain();

    Push(sdl::MakeWindow(id, Kind::WindowClose));
    GameWindow()->Pump();
    Check(rec.closes == 1, "closing the window asks the engine to quit once");
    GameWindow()->Pump();
    GameWindow()->Frame(true);
    Check(rec.closes == 1, "a closed window asks nothing more");
    Drain();

    const int clearsAtDestroy = rec.clearKeys;
    Check(DestroyWindow(game) == TRUE, "the renderer destroys the game window");
    Check(rec.windows.size() == 2 && rec.windows[1] == nullptr, "WM_DESTROY clears g_wv.hWnd");
    Check(GameWindow() == nullptr, "the host let go of the window");
    Check(rec.closes == 1 && rec.clearKeys == clearsAtDestroy, "destroying the window neither quits nor clears keys");

    // --- vid_restart: destroyed while running, then made again -----------------
    const HWND again = MakeWindow();
    Check(again && GameWindow() && rec.windows.size() == 3 && rec.windows[2] == again,
          "a recreated window is adopted again");
    if (!again)
        return;
    const uint32_t againId = IdOf(again);
    GameWindow()->Pump();
    Push(sdl::MakeWindow(againId, Kind::WindowFocusLost));
    Push(sdl::MakeWindow(againId, Kind::WindowFocusGained));
    Push(sdl::MakeKey(againId, SDL_SCANCODE_D, true));
    GameWindow()->Pump();
    rec.lookAnswer = 1;
    GameWindow()->Frame(true);
    Check(GameWindow()->Look(), "the recreated window takes look mode");
    Drain();
    const int closesBefore = rec.closes, clearsBefore2 = rec.clearKeys;
    Check(DestroyWindow(again) == TRUE, "vid_restart destroys a window that is still open");
    Check(rec.closes == closesBefore && rec.clearKeys == clearsBefore2,
          "letting go of an open window neither quits nor clears keys");
    Check(GameWindow() == nullptr && rec.windows.back() == nullptr, "the open window is let go before it goes");
    Check(kisak::win32_window::OpenWindowCount() == 0, "every window made here is destroyed");

    kisak::mp_client::RemoveWindowProcedure();
    const HWND unobserved = MakeWindow();
    Check(unobserved && GameWindow() == nullptr && rec.windows.size() == 4,
          "with the procedure removed, a new window is not adopted");
    if (unobserved)
        DestroyWindow(unobserved);
}

void CheckClientWindowNeedsCallbacks()
{
    ClientWindowEngine missing = Engine();
    missing.mouse = nullptr;
    bool refused = false;
    try { ClientWindow window(nullptr, missing); }
    catch (const std::invalid_argument &) { refused = true; }
    Check(refused, "a game window needs every engine callback");
}

} // namespace

int main()
{
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    try
    {
        CheckCallbacksRequired();
        CheckClientWindowNeedsCallbacks();
        CheckWindowLifetime();
    }
    catch (const std::exception &error)
    {
        ++failures;
        std::printf("FAIL unexpected exception: %s\n", error.what());
    }
    std::printf("mp client window: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
