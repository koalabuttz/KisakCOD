// The multiplayer client's input and window procedure on native Linux: the parts
// of src/win32/win_input.cpp and src/win32/win_wndproc.cpp the engine calls,
// over the game window in mp_client_window.cpp.
//
// What each Windows function does here:
//   * IN_Init registers in_mouse and starts the mouse, IN_Shutdown releases it;
//     in_restart runs both, as on Windows.
//   * IN_Frame hands the frame's motion to CL_MouseEvent while the window is
//     active, and releases the mouse otherwise. The ClickToContinue check is
//     left out: no unit registers that dvar.
//   * IN_ActivateMouse has nothing to start: capture follows CL_MouseEvent's
//     answer in IN_Frame. IN_DeactivateMouse releases it.
//   * IN_ShowSystemCursor, IN_SetCursorPos, IN_IsForegroundWindow and
//     IN_SetForegroundWindow act on the game window.
//   * The window procedure sets g_wv.hWnd on creation and clears it on
//     destruction, runs VID_AppActivate on focus and minimize changes, and turns
//     a close into Com_Quit_f's command.
// Joysticks, MIDI and raw DirectInput are not ported; the Windows build does not
// use them either (IN_StartupMouse never starts DirectInput).
//
// Also here, as on Windows: a keypad digit typed into the console or a menu is
// text only (win_wndproc.cpp's AdustKeyForNumericKeypad), and the clipboard
// (win_main.cpp's Sys_GetClipboardData and Sys_SetClipboardData), through SDL.
//
// With KISAK_LINUX_GAMEPAD, KisakCOD-mp also takes a game controller
// (mp_client_gamepad.cpp): its buttons arrive as keys when the window is
// pumped, and its sticks join the mouse where the window hands the mouse to
// CL_MouseEvent. Without the option nothing below changes.

// KisakCOD-sp links it too: the singleplayer client's input and window
// procedure are the same Windows units built with KISAK_SP.
#if !defined(KISAK_LINUX_MP_CLIENT) && !defined(KISAK_LINUX_SP_GAME)
#error "mp_client_input.cpp is the game client's input: build it in KisakCOD-mp or KisakCOD-sp"
#endif

#include "mp_client_input.h"
#include "mp_client_window.h"
#ifdef KISAK_LINUX_GAMEPAD
#include "mp_client_gamepad.h"
#endif

#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <client/client.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#else
#include <client/cl_input.h>
#endif
#include <sound/snd_public.h>
#include <win32/win_input.h>
#include <win32/win_local.h>

#include <linux/client/window/sdl_api.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace {

const dvar_t *in_mouse;
bool s_mouseInitialized;
qboolean in_appactive;

// win_wndproc.cpp's VID_AppActivate.
void AppActivate(bool active, bool minimized)
{
    g_wv.isMinimized = minimized;
    Key_ClearStates(0);
    g_wv.activeApp = active && !minimized;
    if (g_wv.activeApp)
        Com_TouchMemory();
    IN_Activate(g_wv.activeApp);
}

// WM_CLOSE. Com_Quit_f would shut the renderer down, and with it this window,
// while the window's events are still being pumped; the queued command runs it
// on the next command buffer pass instead.
void CloseWindow()
{
    Key_RemoveCatcher(0, ~KEYCATCH_SCRIPT);
    Cbuf_AddText(0, "quit\n");
}

// win_wndproc.cpp's AdustKeyForNumericKeypad: while the console or a menu has
// the keyboard, a keypad digit is the character it types, not a key.
bool TypingText()
{
    return Key_IsCatcherActive(0, KEYCATCH_CONSOLE | KEYCATCH_UI);
}

// WM_CREATE and WM_DESTROY.
void WindowChanged(void *hwnd)
{
    g_wv.hWnd = static_cast<HWND>(hwnd);
    if (hwnd)
        SND_SetHWND(g_wv.hWnd);
}

kisak::mp_client::ClientWindow *Window()
{
    return kisak::mp_client::GameWindow();
}

} // namespace

void Sys_InstallClientWindow()
{
    // DXVK Native picks its window system (WSI) when the first device is made,
    // from DXVK_WSI_DRIVER. The renderer's windows are SDL3 windows
    // (win32_window_sdl3.cpp), so any other WSI would misread every HWND; left
    // unset, DXVK 2.7.1 throws while choosing one. The probes do the same.
    const char *const wsi = getenv("DXVK_WSI_DRIVER");
    if (!wsi)
    {
        if (setenv("DXVK_WSI_DRIVER", "SDL3", 1) != 0)
            Sys_Error("Could not set DXVK_WSI_DRIVER=SDL3 for the renderer");
    }
    else if (strcmp(wsi, "SDL3") != 0)
    {
        Sys_Error("DXVK_WSI_DRIVER is '%s'; the game window is an SDL3 window, so it must be SDL3 or unset", wsi);
    }
    // The engine handles SIGINT and SIGTERM itself (src/linux/main.cpp); SDL
    // would otherwise replace those handlers when the renderer starts video.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    kisak::mp_client::ClientWindowEngine engine;
#ifdef KISAK_LINUX_GAMEPAD
    engine.mouse = kisak::mp_client::GamepadMouse;
#else
    engine.mouse = CL_MouseEvent;
#endif
    engine.clearKeys = Key_ClearStates;
    engine.activate = AppActivate;
    engine.close = CloseWindow;
    engine.windowChanged = WindowChanged;
    engine.typingText = TypingText;
    kisak::mp_client::InstallWindowProcedure(engine);
}

void Sys_PumpClientWindow()
{
    kisak::mp_client::ClientWindow *const window = Window();
    if (!window)
        return;
    std::string failure;
    try
    {
#ifdef KISAK_LINUX_GAMEPAD
        // Only the window's own thread gets its events, and the controller's.
        if (window->Pump())
            kisak::mp_client::GamepadPump();
#else
        window->Pump();
#endif
    }
    catch (const std::exception &error)
    {
        failure = error.what();
    }
    // Com_Error leaves by longjmp, so it runs outside the handler.
    if (!failure.empty())
        Com_Error(ERR_FATAL, "The game window's events failed: %s", failure.c_str());
}

void IN_Init()
{
    in_mouse = Dvar_RegisterBool("in_mouse", 1, DVAR_ARCHIVE | DVAR_LATCH, "Initialize the mouse drivers");
    s_mouseInitialized = in_mouse->current.enabled;
    if (!s_mouseInitialized)
        Com_Printf(CON_CHANNEL_SYSTEM, "Mouse control not active.\n");
    Dvar_ClearModified((dvar_s *)in_mouse);
#ifdef KISAK_LINUX_GAMEPAD
    kisak::mp_client::GamepadInit();
#endif
}

void IN_Shutdown()
{
    IN_DeactivateMouse();
    s_mouseInitialized = false;
#ifdef KISAK_LINUX_GAMEPAD
    kisak::mp_client::GamepadShutdown();
#endif
}

void IN_Activate(qboolean active)
{
    in_appactive = active;
    if (!active)
        IN_DeactivateMouse();
}

void IN_ActivateMouse(qboolean force)
{
    // Capture is taken in IN_Frame once CL_MouseEvent asks for look mode.
    (void)force;
}

void IN_DeactivateMouse()
{
    if (kisak::mp_client::ClientWindow *const window = Window())
        window->Release();
}

void IN_Frame()
{
    kisak::mp_client::ClientWindow *const window = Window();
    if (window)
        window->Frame(s_mouseInitialized && in_appactive && in_mouse->current.enabled);
#ifdef KISAK_LINUX_GAMEPAD
    kisak::mp_client::GamepadFrame(in_appactive && window && window->Foreground());
#endif
}

void IN_ShowSystemCursor(BOOL show)
{
    if (kisak::mp_client::ClientWindow *const window = Window())
        window->ShowSystemCursor(show != 0);
}

void IN_SetCursorPos(tagPOINT x)
{
    if (kisak::mp_client::ClientWindow *const window = Window())
        window->SetCursorPos(x.x, x.y);
#ifdef KISAK_LINUX_GAMEPAD
    kisak::mp_client::GamepadSetCursor(x.x, x.y);
#endif
}

bool IN_IsForegroundWindow()
{
    kisak::mp_client::ClientWindow *const window = Window();
    return window && window->Foreground();
}

void IN_SetForegroundWindow()
{
    if (kisak::mp_client::ClientWindow *const window = Window())
        window->Raise();
}

// win_main.cpp reads the clipboard as CF_TEXT: bytes in the system's code page,
// cut at the first line break, in a Z_Malloc block the caller frees with
// Com_FreeEvent (Field_Paste, the script watch). SDL hands over UTF-8; the
// engine's text is one byte a character, so Latin-1 characters are kept and
// any other character becomes '?', as a Windows conversion to the ANSI code
// page does.
char *Sys_GetClipboardData()
{
    char *const utf8 = SDL_GetClipboardText();
    if (!utf8)
        return nullptr;
    const unsigned char *in = reinterpret_cast<const unsigned char *>(utf8);
    // Windows: strtok(data, "\n\r\b") ends the text at its first break after
    // any leading ones.
    while (*in == '\n' || *in == '\r' || *in == '\b')
        ++in;
    const size_t length = strlen(reinterpret_cast<const char *>(in));
    char *const data = reinterpret_cast<char *>(Com_AllocEvent(static_cast<int>(length) + 1));
    size_t out = 0;
    while (*in && *in != '\n' && *in != '\r' && *in != '\b')
    {
        const unsigned char lead = *in;
        size_t size = 1;
        if (lead >= 0xC0 && lead <= 0xDF)
            size = 2;
        else if (lead >= 0xE0 && lead <= 0xEF)
            size = 3;
        else if (lead >= 0xF0 && lead <= 0xF7)
            size = 4;
        bool whole = true;
        for (size_t n = 1; n < size; ++n)
        {
            if ((in[n] & 0xC0) != 0x80)
            {
                whole = false;
                size = n;
                break;
            }
        }
        if (size == 1 && lead < 0x80)
            data[out++] = static_cast<char>(lead);
        else if (whole && size == 2 && lead <= 0xC3)
            data[out++] = static_cast<char>(((lead & 0x1F) << 6) | (in[1] & 0x3F));
        else
            data[out++] = '?';
        in += size;
    }
    data[out] = '\0';
    SDL_free(utf8);
    return data;
}

// The engine's bytes are Latin-1; SDL takes UTF-8.
int Sys_SetClipboardData(const char *text)
{
    if (!text)
        return 0;
    std::string utf8;
    utf8.reserve(strlen(text) * 2);
    for (const unsigned char *in = reinterpret_cast<const unsigned char *>(text); *in; ++in)
    {
        if (*in < 0x80)
        {
            utf8 += static_cast<char>(*in);
        }
        else
        {
            utf8 += static_cast<char>(0xC0 | (*in >> 6));
            utf8 += static_cast<char>(0x80 | (*in & 0x3F));
        }
    }
    return SDL_SetClipboardText(utf8.c_str()) ? 1 : 0;
}
