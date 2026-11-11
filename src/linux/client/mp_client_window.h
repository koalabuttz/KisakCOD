#pragma once

// The multiplayer client's game window: what win_wndproc.cpp and the mouse half
// of win_input.cpp do on Windows, over the SDL window the renderer creates.
//
// The renderer makes its window through CreateWindowExA
// (src/linux/client/window/win32_window_sdl3.cpp). InstallWindowProcedure hooks
// that layer's creation and destruction, which stand in for WM_CREATE and
// WM_DESTROY: the window is adopted by a WindowHost when it is created, and let
// go before DestroyWindow destroys it, so a vid_restart or shutdown never leaves
// a host on a dead window. The host never presents to or destroys the window.
//
// Events, as the Windows window procedure delivers them:
//   * keys, mouse buttons and the wheel go into the engine queue as SE_KEY, and
//     text as SE_CHAR (EngineInput, the same adapter the window tests check);
//   * focus and minimize changes reach the engine as VID_AppActivate does, after
//     the held keys are released;
//   * closing the window asks the engine to quit, through the command buffer.
//     Windows quits from inside the window procedure; here quitting would
//     destroy the window while its own events are being pumped, so it waits for
//     the next command buffer pass.
//
// The mouse follows win_input.cpp's IN_MouseMove: motion is gathered while
// events are pumped and handed to the engine once per frame (Frame). When the
// engine answers that it wants look mode (CL_MouseEvent returns nonzero, which
// is when Windows recentres the cursor), the window takes relative capture; when
// it answers no (a menu or the console has the mouse), capture is released and
// the system cursor follows the engine's show/hide requests. Positions are
// handed over in window pixels, the units D3D9 renders in.
//
// Single thread: the thread that creates the window pumps it and destroys it.

#include <linux/client/window/window_host.h>

#include <memory>

namespace kisak::mp_client {

// What the window hands to the engine. src/linux/client/mp_client_input.cpp
// wires these to the engine; tests pass recorders.
struct ClientWindowEngine {
    // CL_MouseEvent: position in window pixels, motion since the last frame.
    // Nonzero asks for look mode.
    int (*mouse)(int x, int y, int dx, int dy) = nullptr;
    // Key_ClearStates.
    void (*clearKeys)(int localClientNum) = nullptr;
    // VID_AppActivate: focus or minimize changed.
    void (*activate)(bool active, bool minimized) = nullptr;
    // WM_CLOSE: the user closed the window.
    void (*close)() = nullptr;
    // WM_CREATE / WM_DESTROY: the game window's handle, or nullptr once it is
    // going away (g_wv.hWnd).
    void (*windowChanged)(void *hwnd) = nullptr;
    // Optional. True while the console or a menu has the keyboard, so typed
    // keypad digits go in as text only (EventSink::TypingText).
    bool (*typingText)() = nullptr;
};

class ClientWindow {
public:
    // `window` is borrowed and must outlive this object. Every callback in
    // `engine` is required, except typingText.
    ClientWindow(SDL_Window *window, const ClientWindowEngine &engine);
    ~ClientWindow();
    ClientWindow(const ClientWindow &) = delete;
    ClientWindow &operator=(const ClientWindow &) = delete;

    // Pump the window's pending events into the engine queue. Only the thread
    // that created the window gets them, as with Windows' per-thread message
    // queues; another thread's call does nothing and returns false.
    bool Pump();
    // IN_Frame: hand the gathered motion to the engine and follow its answer.
    // `mouseEnabled` is win_input.cpp's "mouse initialized and app active".
    void Frame(bool mouseEnabled);
    // IN_DeactivateMouse: release capture and drop gathered motion.
    void Release();
    // IN_ShowSystemCursor. Recorded while the engine has look mode, applied
    // once a menu or the console takes the mouse.
    void ShowSystemCursor(bool show);
    // IN_IsForegroundWindow / IN_SetForegroundWindow.
    bool Foreground() const;
    void Raise();
    // IN_SetCursorPos, in window pixels.
    void SetCursorPos(int x, int y);

    bool Look() const { return look_; }
    bool CursorHidden() const { return hideCursor_; }
    const window::WindowHost &Host() const { return *host_; }

private:
    class Sink;
    void SetLook(bool look);
    void ToPixels(int &x, int &y) const;
    ClientWindowEngine engine_;
    SDL_Window *window_;
    std::unique_ptr<Sink> sink_;
    std::unique_ptr<window::WindowHost> host_;
    bool look_ = false;
    bool hideCursor_ = false;
};

// Hook the Win32 window layer so the next window CreateWindowExA makes becomes
// the game window. One game window at a time, as the renderer makes. Copies
// `engine`. Remove before the callbacks' owner goes away.
void InstallWindowProcedure(const ClientWindowEngine &engine);
void RemoveWindowProcedure();
// The adopted game window, or nullptr when there is none or adopting failed.
ClientWindow *GameWindow();

} // namespace kisak::mp_client
