// The Win32 window, monitor and process calls the renderer makes
// (src/linux/win32_window.h), for the PS Vita multiplayer client.
//
// The Vita has one screen, 960 x 544, and no window system. CreateWindowExA
// hands out that screen as the one game window: a token the D3D9 back end
// recognises, never dereferenced. The renderer asks for the window it wants
// (vid_xpos/ypos, r_mode); whatever it asks, the screen is the window, and the
// one monitor is the screen.

#if !defined(__vita__) || !defined(KISAK_VITA_MP_CLIENT)
#error "vita_win32_window.cpp is the PS Vita client's window layer"
#endif

#include "vita_client.h"

#include <linux/d3d9_types.h>

#include <cstdint>
#include <cstdio>

namespace {

// The one window and the one monitor. Their addresses are the handles.
char g_screenWindow;
char g_screenMonitor;
bool g_windowOpen = false;

HWND ScreenWindow()
{
    return reinterpret_cast<HWND>(&g_screenWindow);
}

HMONITOR ScreenMonitor()
{
    return reinterpret_cast<HMONITOR>(&g_screenMonitor);
}

tagRECT ScreenRect()
{
    tagRECT rect{};
    rect.left = 0;
    rect.top = 0;
    rect.right = kVitaScreenWidth;
    rect.bottom = kVitaScreenHeight;
    return rect;
}

} // namespace

bool Vita_IsScreenWindow(const void *hwnd)
{
    return g_windowOpen && hwnd == &g_screenWindow;
}

HWND WINAPI CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int nWidth, int nHeight, HWND hWndParent,
                            HMENU hMenu, HINSTANCE, LPVOID lpParam)
{
    if (hWndParent || hMenu || lpParam)
    {
        Vita_ClientLog("vita window: CreateWindowExA supports the top-level game window only");
        return nullptr;
    }
    if (g_windowOpen)
    {
        Vita_ClientLog("vita window: the screen already holds a window");
        return nullptr;
    }
    if (nWidth != kVitaScreenWidth || nHeight != kVitaScreenHeight)
    {
        char line[128];
        std::snprintf(line, sizeof(line), "vita window: asked for %d x %d, the screen is %d x %d", nWidth, nHeight,
                      kVitaScreenWidth, kVitaScreenHeight);
        Vita_ClientLog(line);
    }
    g_windowOpen = true;
    // WM_CREATE: the window procedure hears about the window before the caller.
    Vita_ClientWindowChanged(ScreenWindow());
    return ScreenWindow();
}

BOOL WINAPI ShowWindow(HWND hWnd, int)
{
    // The screen is always shown; Windows answers whether it was visible before.
    return Vita_IsScreenWindow(hWnd) ? TRUE : FALSE;
}

BOOL WINAPI IsWindow(HWND hWnd)
{
    return Vita_IsScreenWindow(hWnd) ? TRUE : FALSE;
}

BOOL WINAPI DestroyWindow(HWND hWnd)
{
    if (!Vita_IsScreenWindow(hWnd))
        return FALSE;
    // WM_DESTROY: the device and the window procedure let go first.
    Vita_RendererWindowDestroying(hWnd);
    Vita_ClientWindowChanged(nullptr);
    g_windowOpen = false;
    return TRUE;
}

BOOL WINAPI AdjustWindowRectEx(tagRECT *lpRect, DWORD, BOOL, DWORD)
{
    return lpRect ? TRUE : FALSE;
}

int WINAPI GetSystemMetrics(int nIndex)
{
    if (nIndex == SM_CXSCREEN)
        return kVitaScreenWidth;
    if (nIndex == SM_CYSCREEN)
        return kVitaScreenHeight;
    return 0;
}

HWND WINAPI GetActiveWindow()
{
    return g_windowOpen ? ScreenWindow() : nullptr;
}

HMONITOR WINAPI MonitorFromWindow(HWND hwnd, DWORD)
{
    return Vita_IsScreenWindow(hwnd) ? ScreenMonitor() : nullptr;
}

BOOL WINAPI ClientToScreen(HWND hWnd, tagPOINT *lpPoint)
{
    // The window starts at the screen's corner.
    return Vita_IsScreenWindow(hWnd) && lpPoint ? TRUE : FALSE;
}

HMONITOR WINAPI MonitorFromPoint(tagPOINT pt, DWORD dwFlags)
{
    const bool inside = pt.x >= 0 && pt.y >= 0 && pt.x < kVitaScreenWidth && pt.y < kVitaScreenHeight;
    if (inside || dwFlags == MONITOR_DEFAULTTOPRIMARY || dwFlags == MONITOR_DEFAULTTONEAREST)
        return ScreenMonitor();
    return nullptr;
}

BOOL WINAPI EnumDisplayMonitors(HDC hdc, const tagRECT *lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData)
{
    if (hdc || lprcClip || !lpfnEnum)
        return FALSE;
    tagRECT rect = ScreenRect();
    return lpfnEnum(ScreenMonitor(), nullptr, &rect, dwData) ? TRUE : FALSE;
}

BOOL WINAPI GetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFO lpmi)
{
    if (!lpmi || lpmi->cbSize < sizeof(MONITORINFO) || hMonitor != ScreenMonitor())
        return FALSE;
    lpmi->rcMonitor = ScreenRect();
    lpmi->rcWork = ScreenRect();
    lpmi->dwFlags = MONITORINFOF_PRIMARY;
    return TRUE;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName)
{
    static char processModuleToken;
    return lpModuleName ? nullptr : reinterpret_cast<HMODULE>(&processModuleToken);
}

int WINAPI MessageBoxA(HWND, LPCSTR lpText, LPCSTR lpCaption, UINT)
{
    char line[1024];
    std::snprintf(line, sizeof(line), "%s: %s", lpCaption ? lpCaption : "Message", lpText ? lpText : "");
    Vita_ClientLog(line);
    return IDOK;
}

HINSTANCE WINAPI ShellExecuteA(HWND, LPCSTR lpOperation, LPCSTR lpFile, LPCSTR, LPCSTR, int)
{
    char line[512];
    std::snprintf(line, sizeof(line), "vita window: ShellExecuteA refused (%s %s): nothing is run on the Vita",
                  lpOperation ? lpOperation : "open", lpFile ? lpFile : "");
    Vita_ClientLog(line);
    // SE_ERR_ACCESSDENIED: values of 32 and below are failures.
    return reinterpret_cast<HINSTANCE>(static_cast<std::uintptr_t>(5));
}
