// win32_window.h -- the Win32 window, monitor and process calls that the
// production renderer units make, declared for the native Linux D3D9 boundary.
//
// Reached only as the last section of src/linux/d3d9_native.h, so a unit sees
// these names only when it opted into KISAK_LINUX_NATIVE_D3D9. The dedicated
// server and every stand-in unit never do. Declaring a name here promises
// nothing by itself: only a target that links
// src/linux/client/window/win32_window_sdl3.cpp can call these functions, and
// that file implements them over SDL3 with DXVK Native's SDL3 WSI handle
// encoding, so a handle the engine passes to D3D9 is one DXVK can use:
//
//   HWND      SDL_Window *, cast           (dxvk::wsi::toHwnd)
//   HMONITOR  SDL_DisplayID, as a pointer   (dxvk::wsi::toHmonitor)
//
// The declarations keep the Windows signatures and the stdcall convention
// (WINAPI/CALLBACK from compat.h), so engine code such as
// `EnumDisplayMonitors(0, 0, (MONITORENUMPROC)R_MonitorEnumCallback, ...)` in
// src/gfx_d3d/r_init.cpp compiles unmodified. Only what the renderer units call
// is here; add a declaration together with its implementation and a test.
//
// Sleep is here too, because the renderer's device retry loops call it. It is
// inline: a POSIX sleep needs no window system.

#pragma once

#if !defined(KISAK_LINUX_NATIVE_D3D9) || !defined(KISAK_LINUX_D3D9_TYPES_DISPATCH)
#error "win32_window.h is part of the native D3D9 boundary: include <linux/d3d9_types.h> in a KISAK_LINUX_NATIVE_D3D9 unit"
#endif

#include <linux/compat.h>

#include <cerrno>
#include <cstddef>
#include <sched.h>
#include <time.h>
#include <type_traits>

// RECT and POINT are the Windows layouts the boundary established (tagRECT from
// DXVK's windows_base.h under the Windows tag, tagPOINT from compat.h).
static_assert(sizeof(tagRECT) == 16 && sizeof(tagPOINT) == 8, "Win32 RECT and POINT layouts");

struct HMENU__;
typedef struct HMENU__ *HMENU;

// --- Window styles and commands ----------------------------------------------
inline constexpr DWORD WS_EX_TOPMOST = 0x00000008u;
inline constexpr DWORD WS_POPUP = 0x80000000u;
inline constexpr DWORD WS_VISIBLE = 0x10000000u;
inline constexpr DWORD WS_CAPTION = 0x00C00000u;
inline constexpr DWORD WS_SYSMENU = 0x00080000u;

inline constexpr int SW_HIDE = 0;
inline constexpr int SW_SHOWNORMAL = 1;
inline constexpr int SW_SHOW = 5;

inline constexpr int SM_CXSCREEN = 0;
inline constexpr int SM_CYSCREEN = 1;

HWND WINAPI CreateWindowExA(DWORD dwExStyle,
                            LPCSTR lpClassName,
                            LPCSTR lpWindowName,
                            DWORD dwStyle,
                            int X,
                            int Y,
                            int nWidth,
                            int nHeight,
                            HWND hWndParent,
                            HMENU hMenu,
                            HINSTANCE hInstance,
                            LPVOID lpParam);
BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow);
BOOL WINAPI IsWindow(HWND hWnd);
BOOL WINAPI DestroyWindow(HWND hWnd);
BOOL WINAPI AdjustWindowRectEx(tagRECT *lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
int WINAPI GetSystemMetrics(int nIndex);
HWND WINAPI GetActiveWindow();
// r_screenshot.cpp turns the client-area origin into desktop coordinates.
BOOL WINAPI ClientToScreen(HWND hWnd, tagPOINT *lpPoint);

// --- Monitors ------------------------------------------------------------------
inline constexpr DWORD MONITOR_DEFAULTTONULL = 0u;
inline constexpr DWORD MONITOR_DEFAULTTOPRIMARY = 1u;
inline constexpr DWORD MONITOR_DEFAULTTONEAREST = 2u;
inline constexpr DWORD MONITORINFOF_PRIMARY = 1u;

struct tagMONITORINFO
{
    DWORD cbSize;
    tagRECT rcMonitor;
    tagRECT rcWork;
    DWORD dwFlags;
};
typedef tagMONITORINFO MONITORINFO, *LPMONITORINFO;
// r_init.cpp and r_screenshot.cpp set cbSize to the literal 40.
static_assert(sizeof(tagMONITORINFO) == 40 && offsetof(tagMONITORINFO, rcWork) == 20, "Win32 MONITORINFO layout");

typedef BOOL(CALLBACK *MONITORENUMPROC)(HMONITOR, HDC, tagRECT *, LPARAM);

HMONITOR WINAPI MonitorFromPoint(tagPOINT pt, DWORD dwFlags);
// r_screenshot.cpp asks which monitor shows the game window.
HMONITOR WINAPI MonitorFromWindow(HWND hwnd, DWORD dwFlags);
BOOL WINAPI EnumDisplayMonitors(HDC hdc, const tagRECT *lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData);
BOOL WINAPI GetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFO lpmi);

// --- Process and shell -----------------------------------------------------------
inline constexpr UINT MB_OK = 0x00000000u;
inline constexpr UINT MB_ICONERROR = 0x00000010u;
inline constexpr int IDOK = 1;

HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
int WINAPI MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
HINSTANCE WINAPI ShellExecuteA(HWND hwnd,
                               LPCSTR lpOperation,
                               LPCSTR lpFile,
                               LPCSTR lpParameters,
                               LPCSTR lpDirectory,
                               int nShowCmd);

// Sleep(0) gives up the rest of the time slice, like the Windows call; any other
// value sleeps at least that many milliseconds, resuming after signals.
inline void WINAPI Sleep(DWORD dwMilliseconds)
{
    if (dwMilliseconds == 0)
    {
        sched_yield();
        return;
    }
    timespec remaining{static_cast<time_t>(dwMilliseconds / 1000u),
                       static_cast<long>(dwMilliseconds % 1000u) * 1000000L};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
    {
    }
}

// The engine's callbacks and these calls keep the Windows stdcall convention.
static_assert(std::is_same_v<MONITORENUMPROC, BOOL(__attribute__((stdcall)) *)(HMONITOR, HDC, tagRECT *, LPARAM)>,
              "MONITORENUMPROC is a stdcall callback, like R_MonitorEnumCallback");
static_assert(std::is_same_v<decltype(&MonitorFromPoint), HMONITOR(__attribute__((stdcall)) *)(tagPOINT, DWORD)>,
              "the Win32 calls are declared stdcall");
