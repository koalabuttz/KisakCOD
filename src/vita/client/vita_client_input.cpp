// The PS Vita multiplayer client's window procedure and input: the parts of
// src/win32/win_input.cpp and src/win32/win_wndproc.cpp the engine calls (the
// same set src/linux/client/mp_client_input.cpp provides on Linux).
//
// Stage 1 (docs/vita-client.md): the window procedure sets g_wv.hWnd and makes
// the app active; the buttons and sticks are not mapped to the engine yet.
// START + SELECT quits, from the main loop (Vita_QuitRequested).

#include "vita_client.h"

#include <linux/client/mp_client_input.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <client/client.h>
#include <client_mp/client_mp.h>
#include <sound/snd_public.h>
#include <win32/win_input.h>
#include <win32/win_local.h>

namespace {

const dvar_t *in_mouse;
qboolean in_appactive;

} // namespace

void Vita_ClientLog(const char *line)
{
    Com_Printf(CON_CHANNEL_SYSTEM, "%s\n", line);
}

void Vita_ClientWindowChanged(void *hwnd)
{
    g_wv.hWnd = static_cast<HWND>(hwnd);
    if (!hwnd)
        return;
    SND_SetHWND(g_wv.hWnd);
    // The screen is always in front: win_wndproc.cpp's VID_AppActivate(true).
    g_wv.isMinimized = false;
    g_wv.activeApp = true;
    IN_Activate(true);
}

void Sys_InstallClientWindow()
{
}

void Sys_PumpClientWindow()
{
}

void IN_Init()
{
    in_mouse = Dvar_RegisterBool("in_mouse", 1, DVAR_ARCHIVE | DVAR_LATCH, "Initialize the mouse drivers");
    Dvar_ClearModified((dvar_s *)in_mouse);
}

void IN_Shutdown()
{
}

void IN_Activate(qboolean active)
{
    in_appactive = active;
}

void IN_ActivateMouse(qboolean)
{
}

void IN_DeactivateMouse()
{
}

void IN_Frame()
{
}

void IN_ShowSystemCursor(BOOL)
{
}

void IN_SetCursorPos(tagPOINT)
{
}

bool IN_IsForegroundWindow()
{
    return g_wv.hWnd != nullptr;
}

void IN_SetForegroundWindow()
{
}
