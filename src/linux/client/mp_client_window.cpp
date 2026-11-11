// The multiplayer client's game window. See mp_client_window.h.

#include "mp_client_window.h"

#include <linux/client/window/engine_input.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/window/win32_window_sdl3.h>

#if KISAK_LINUX_SDL_MAJOR != 3
#error "the multiplayer client's window follows the renderer's SDL3 window: build it with KISAK_LINUX_SDL_MAJOR=3"
#endif

#include <climits>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace kisak::mp_client {

namespace {

// The sink that is letting go of its window. Its teardown releases held keys
// and runs the quit path; neither may reach the engine then, because the window
// is going away under a vid_restart or a shutdown, not being closed by the user.
bool g_detaching = false;
ClientWindowEngine g_clearKeysOwner;

int Saturate(long long value)
{
    return value > INT_MAX ? INT_MAX : value < INT_MIN ? INT_MIN : static_cast<int>(value);
}

} // namespace

class ClientWindow::Sink final : public window::EngineInput {
public:
    explicit Sink(const ClientWindowEngine &engine)
        : EngineInput(nullptr, ClearKeys), engine_(engine)
    {
        g_clearKeysOwner = engine;
    }
    void Attach(const window::WindowHost &host) { host_ = &host; }

    // Motion is gathered, not queued: the engine takes it once per frame, as
    // win_input.cpp reads the cursor once per frame.
    void Mouse(uint32_t, int x, int y, int dx, int dy) override
    {
        x_ = x;
        y_ = y;
        dx_ = Saturate(static_cast<long long>(dx_) + dx);
        dy_ = Saturate(static_cast<long long>(dy_) + dy);
    }
    // The renderer owns the window size; a resize reaches it through vid_restart.
    void Resize(int, int) override {}

    struct Motion { int x, y, dx, dy; };
    Motion Take()
    {
        const Motion motion{x_, y_, dx_, dy_};
        dx_ = dy_ = 0;
        return motion;
    }
    void Drop() { dx_ = dy_ = 0; }

private:
    static void ClearKeys(int localClientNum)
    {
        if (!g_detaching)
            g_clearKeysOwner.clearKeys(localClientNum);
    }
    void OnFocus(bool focused) override
    {
        if (g_detaching)
            return;
        const bool minimized = host_ && host_->Minimized();
        engine_.activate(focused && !minimized, minimized);
    }
    void OnQuit() override
    {
        if (!g_detaching)
            engine_.close();
    }
    bool TypingText() const override
    {
        return engine_.typingText && engine_.typingText();
    }

    const ClientWindowEngine &engine_;
    const window::WindowHost *host_ = nullptr;
    int x_ = 0, y_ = 0, dx_ = 0, dy_ = 0;
};

ClientWindow::ClientWindow(SDL_Window *window, const ClientWindowEngine &engine)
    : engine_(engine), window_(window)
{
    if (!engine_.mouse || !engine_.clearKeys || !engine_.activate || !engine_.close || !engine_.windowChanged)
        throw std::invalid_argument("The game window needs every engine callback");
    sink_ = std::make_unique<Sink>(engine_);
    host_ = std::make_unique<window::WindowHost>(*sink_, window::AdoptedWindow{window});
    sink_->Attach(*host_);
    // A window can already have the keyboard focus when it is adopted: SDL
    // gives a shown window the focus at once where no window manager does it
    // later (X11 with none, as under Xvfb; the dummy driver). The host then
    // counts the window as focused and the queued focus event changes
    // nothing, so the engine would never hear of it and VID_AppActivate would
    // never run: no mouse, no menu clicks. Windows sends WM_ACTIVATE when the
    // window is shown; this is that.
    if (host_->Focused() && !host_->Minimized())
        engine_.activate(true, false);
}

ClientWindow::~ClientWindow()
{
    const bool wasDetaching = g_detaching;
    g_detaching = true;
    host_.reset();
    sink_.reset();
    g_detaching = wasDetaching;
}

bool ClientWindow::Pump()
{
    // PeekMessage only returns messages for windows the calling thread owns.
    // The render thread reaches Win_GetEvent too, through Sys_LoadingKeepAlive
    // in SCR_UpdateScreen while it draws the loading screen, and on Windows it
    // gets none of the game window's messages there. Likewise here.
    if (!host_->OnOwnerThread())
        return false;
    host_->Pump(0);
    return true;
}

void ClientWindow::ToPixels(int &x, int &y) const
{
    int logicalWidth = 0, logicalHeight = 0;
    SDL_GetWindowSize(window_, &logicalWidth, &logicalHeight);
    const window::PixelExtent pixels = host_->SurfaceExtent();
    if (logicalWidth > 0 && logicalHeight > 0 && (pixels.width != logicalWidth || pixels.height != logicalHeight))
    {
        x = Saturate(static_cast<long long>(x) * pixels.width / logicalWidth);
        y = Saturate(static_cast<long long>(y) * pixels.height / logicalHeight);
    }
}

void ClientWindow::Frame(bool mouseEnabled)
{
    if (!host_->Running())
        return;
    if (!mouseEnabled || !host_->Focused() || host_->Minimized())
    {
        Release();
        return;
    }
    Sink::Motion motion = sink_->Take();
    ToPixels(motion.x, motion.y);
    SetLook(engine_.mouse(motion.x, motion.y, motion.dx, motion.dy) != 0);
}

void ClientWindow::SetLook(bool look)
{
    if (look == look_ || !host_->Running())
        return;
    if (look)
    {
        // Relative capture hides the cursor itself and cannot share it.
        host_->SetEngineCursor(false);
        if (!host_->SetRelativeMouse(true))
            std::fprintf(stderr, "KisakCOD-mp: the window could not capture the mouse: %s\n",
                         host_->RelativeMouseError().c_str());
    }
    else
    {
        host_->SetRelativeMouse(false);
        host_->SetEngineCursor(hideCursor_);
    }
    look_ = look;
}

void ClientWindow::Release()
{
    sink_->Drop();
    SetLook(false);
}

void ClientWindow::ShowSystemCursor(bool show)
{
    hideCursor_ = !show;
    if (!look_ && host_->Running())
        host_->SetEngineCursor(hideCursor_);
}

bool ClientWindow::Foreground() const
{
    return host_->Running() && host_->Focused();
}

void ClientWindow::Raise()
{
    if (host_->Running())
        SDL_RaiseWindow(window_);
}

void ClientWindow::SetCursorPos(int x, int y)
{
    if (!host_->Running())
        return;
    int logicalWidth = 0, logicalHeight = 0;
    SDL_GetWindowSize(window_, &logicalWidth, &logicalHeight);
    const window::PixelExtent pixels = host_->SurfaceExtent();
    float logicalX = static_cast<float>(x), logicalY = static_cast<float>(y);
    if (pixels.width > 0 && pixels.height > 0)
    {
        logicalX = logicalX * logicalWidth / pixels.width;
        logicalY = logicalY * logicalHeight / pixels.height;
    }
    SDL_WarpMouseInWindow(window_, logicalX, logicalY);
}

// ---- the window procedure ------------------------------------------------------

namespace {

ClientWindowEngine g_procedureEngine;
ClientWindow *g_window = nullptr;
void *g_gameHwnd = nullptr;

void Created(void *hwnd)
{
    if (g_gameHwnd)
    {
        std::fprintf(stderr, "KisakCOD-mp: a second window was created while the game window exists; it gets no input\n");
        return;
    }
    g_gameHwnd = hwnd;
    try
    {
        g_window = new ClientWindow(static_cast<SDL_Window *>(hwnd), g_procedureEngine);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "KisakCOD-mp: the game window gets no input: %s\n", error.what());
    }
    g_procedureEngine.windowChanged(hwnd);
}

void Destroying(void *hwnd)
{
    if (!hwnd || hwnd != g_gameHwnd)
        return;
    // The window's own events stop before anything else hears it is gone.
    delete g_window;
    g_window = nullptr;
    g_gameHwnd = nullptr;
    g_procedureEngine.windowChanged(nullptr);
}

const win32_window::WindowObserver kObserver{Created, Destroying};

} // namespace

void InstallWindowProcedure(const ClientWindowEngine &engine)
{
    if (!engine.mouse || !engine.clearKeys || !engine.activate || !engine.close || !engine.windowChanged)
        throw std::invalid_argument("The window procedure needs every engine callback");
    g_procedureEngine = engine;
    win32_window::SetWindowObserver(&kObserver);
}

void RemoveWindowProcedure()
{
    win32_window::SetWindowObserver(nullptr);
}

ClientWindow *GameWindow()
{
    return g_window;
}

} // namespace kisak::mp_client
