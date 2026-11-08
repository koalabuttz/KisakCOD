// win32_window_sdl3.cpp -- the Win32 calls declared in src/linux/win32_window.h,
// implemented over SDL3 for the native D3D9 renderer.
//
// The handles follow DXVK Native's SDL3 WSI (wsi/native_sdl3.h): an HWND is the
// SDL_Window * itself and an HMONITOR is the SDL_DisplayID value. So the window
// r_init.cpp creates is the one DXVK presents to, and the monitor it picks
// compares equal to IDirect3D9::GetAdapterMonitor's answer for the same display.
// DXVK loads libSDL3.so.0 itself; this file links the same library, so both
// sides share one SDL instance.
//
// Behaviour that differs from Windows, on purpose:
//   * Window classes do not exist. The class name is accepted and ignored, and
//     there is no window procedure. A process can install one observer for
//     creation and destruction instead (SetWindowObserver), which every window
//     reports to; the multiplayer client uses it as its WM_CREATE/WM_DESTROY.
//   * Window sizes are client sizes: the window manager draws the decorations,
//     so AdjustWindowRectEx leaves the rectangle as it is.
//   * Only top-level windows. A parent, a menu or a creation parameter fails the
//     call, because nothing here could honour them.
//   * MessageBoxA writes to stderr and shows no dialog: a modal dialog would hang
//     an unattended run. ShellExecuteA never runs anything and always fails.
//   * GetModuleHandleA(nullptr) is a fixed non-null token; there is no module
//     handle to give, and CreateWindowExA ignores it. Named modules are not found.
//
// SDL's video subsystem is initialized on first use and kept for the process's
// lifetime. Window calls belong on the thread that created the windows, as on
// Windows; the bookkeeping below is locked anyway, so IsWindow is safe anywhere.

#if !defined(KISAK_LINUX_NATIVE_D3D9)
#error "win32_window_sdl3.cpp implements the native D3D9 boundary's Win32 calls: build it through kisak_linux_native_d3d9"
#endif

#include <linux/d3d9_types.h>

#include "sdl_api.h"
#include "win32_window_sdl3.h"

#if KISAK_LINUX_SDL_MAJOR != 3
#error "the Win32 window layer follows DXVK Native's SDL3 WSI: build it with KISAK_LINUX_SDL_MAJOR=3"
#endif

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_set>

namespace {

std::mutex g_windowsLock;
std::unordered_set<SDL_Window *> g_windows;
bool g_videoReferenced = false;
kisak::win32_window::WindowObserver g_observer;
void (*g_rendererDestroying)(void *hwnd) = nullptr;

kisak::win32_window::WindowObserver Observer()
{
    std::lock_guard<std::mutex> lock(g_windowsLock);
    return g_observer;
}

void (*RendererDestroying())(void *hwnd)
{
    std::lock_guard<std::mutex> lock(g_windowsLock);
    return g_rendererDestroying;
}

bool EnsureVideo()
{
    std::lock_guard<std::mutex> lock(g_windowsLock);
    if (g_videoReferenced)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "win32_window: SDL video could not start: %s\n", SDL_GetError());
        return false;
    }
    g_videoReferenced = true;
    return true;
}

bool VideoStarted()
{
    std::lock_guard<std::mutex> lock(g_windowsLock);
    return g_videoReferenced;
}

SDL_Window *KnownWindow(HWND hwnd)
{
    SDL_Window *const window = reinterpret_cast<SDL_Window *>(hwnd);
    std::lock_guard<std::mutex> lock(g_windowsLock);
    return window && g_windows.count(window) ? window : nullptr;
}

HMONITOR ToMonitor(SDL_DisplayID display)
{
    return reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(display));
}

SDL_DisplayID ToDisplay(HMONITOR monitor)
{
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(monitor);
    return value <= UINT32_MAX ? static_cast<SDL_DisplayID>(value) : 0;
}

tagRECT ToRect(const SDL_Rect &rect)
{
    tagRECT result{};
    result.left = rect.x;
    result.top = rect.y;
    result.right = rect.x + rect.w;
    result.bottom = rect.y + rect.h;
    return result;
}

bool DisplayExists(SDL_DisplayID display)
{
    int count = 0;
    SDL_DisplayID *const displays = SDL_GetDisplays(&count);
    bool found = false;
    for (int index = 0; displays && index < count; ++index)
        found = found || displays[index] == display;
    SDL_free(displays);
    return display != 0 && found;
}

} // namespace

namespace kisak::win32_window {

std::uint64_t WindowFlagsForStyle(std::uint32_t style, std::uint32_t exStyle)
{
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (!(style & WS_VISIBLE))
        flags |= SDL_WINDOW_HIDDEN;
    if (style & WS_POPUP)
        flags |= SDL_WINDOW_BORDERLESS;
    if (exStyle & WS_EX_TOPMOST)
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    return flags;
}

std::size_t OpenWindowCount()
{
    std::lock_guard<std::mutex> lock(g_windowsLock);
    return g_windows.size();
}

void SetWindowObserver(const WindowObserver *observer)
{
    std::lock_guard<std::mutex> lock(g_windowsLock);
    g_observer = observer ? *observer : WindowObserver{};
}

void SetRendererWindowDestroying(void (*destroying)(void *hwnd))
{
    std::lock_guard<std::mutex> lock(g_windowsLock);
    g_rendererDestroying = destroying;
}

} // namespace kisak::win32_window

HWND WINAPI CreateWindowExA(DWORD dwExStyle,
                            LPCSTR,
                            LPCSTR lpWindowName,
                            DWORD dwStyle,
                            int X,
                            int Y,
                            int nWidth,
                            int nHeight,
                            HWND hWndParent,
                            HMENU hMenu,
                            HINSTANCE,
                            LPVOID lpParam)
{
    if (hWndParent || hMenu || lpParam)
    {
        std::fprintf(stderr, "win32_window: CreateWindowExA supports top-level windows only\n");
        return nullptr;
    }
    if (nWidth <= 0 || nHeight <= 0)
    {
        std::fprintf(stderr, "win32_window: CreateWindowExA needs a positive size, got %d x %d\n", nWidth, nHeight);
        return nullptr;
    }
    if (!EnsureVideo())
        return nullptr;

    const SDL_WindowFlags flags = kisak::win32_window::WindowFlagsForStyle(dwStyle, dwExStyle);
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (!properties)
    {
        std::fprintf(stderr, "win32_window: SDL_CreateProperties failed: %s\n", SDL_GetError());
        return nullptr;
    }
    SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, lpWindowName ? lpWindowName : "");
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, X);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, Y);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, nWidth);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, nHeight);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
    SDL_Window *const window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!window)
    {
        std::fprintf(stderr, "win32_window: SDL could not create a %d x %d Vulkan window: %s\n", nWidth, nHeight,
                     SDL_GetError());
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_windowsLock);
        g_windows.insert(window);
    }
    // WM_CREATE: the window procedure hears about the window before the caller.
    if (const auto created = Observer().created)
        created(window);
    return reinterpret_cast<HWND>(window);
}

BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow)
{
    SDL_Window *const window = KnownWindow(hWnd);
    if (!window)
        return FALSE;
    const BOOL wasVisible = (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) ? FALSE : TRUE;
    if (nCmdShow == SW_HIDE)
        SDL_HideWindow(window);
    else
        SDL_ShowWindow(window);
    return wasVisible;
}

BOOL WINAPI IsWindow(HWND hWnd)
{
    return KnownWindow(hWnd) ? TRUE : FALSE;
}

BOOL WINAPI DestroyWindow(HWND hWnd)
{
    SDL_Window *const window = reinterpret_cast<SDL_Window *>(hWnd);
    // WM_DESTROY: whoever borrowed the window lets go while it still exists.
    if (KnownWindow(hWnd))
    {
        if (const auto destroying = Observer().destroying)
            destroying(window);
        if (const auto destroying = RendererDestroying())
            destroying(window);
    }
    {
        std::lock_guard<std::mutex> lock(g_windowsLock);
        if (!window || !g_windows.erase(window))
            return FALSE;
    }
    SDL_DestroyWindow(window);
    return TRUE;
}

BOOL WINAPI AdjustWindowRectEx(tagRECT *lpRect, DWORD, BOOL, DWORD)
{
    return lpRect ? TRUE : FALSE;
}

int WINAPI GetSystemMetrics(int nIndex)
{
    if ((nIndex != SM_CXSCREEN && nIndex != SM_CYSCREEN) || !EnsureVideo())
        return 0;
    const SDL_DisplayMode *const mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!mode)
        return 0;
    return nIndex == SM_CXSCREEN ? mode->w : mode->h;
}

HWND WINAPI GetActiveWindow()
{
    if (!VideoStarted())
        return nullptr;
    SDL_Window *const focus = SDL_GetKeyboardFocus();
    return KnownWindow(reinterpret_cast<HWND>(focus)) ? reinterpret_cast<HWND>(focus) : nullptr;
}

// Windows reports ERROR_INVALID_WINDOW_HANDLE and no monitor for a handle that is
// not a window, whatever dwFlags says; a window on no display follows dwFlags.
HMONITOR WINAPI MonitorFromWindow(HWND hwnd, DWORD dwFlags)
{
    SDL_Window *const window = KnownWindow(hwnd);
    if (!window)
        return nullptr;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display)
        return ToMonitor(display);
    if (dwFlags == MONITOR_DEFAULTTOPRIMARY || dwFlags == MONITOR_DEFAULTTONEAREST)
        return ToMonitor(SDL_GetPrimaryDisplay());
    return nullptr;
}

// SDL3 reports the client area's position, which is what ClientToScreen adds.
// Where the window system gives no global position (Wayland), SDL answers 0,0.
BOOL WINAPI ClientToScreen(HWND hWnd, tagPOINT *lpPoint)
{
    SDL_Window *const window = KnownWindow(hWnd);
    if (!window || !lpPoint)
        return FALSE;
    int x = 0;
    int y = 0;
    if (!SDL_GetWindowPosition(window, &x, &y))
        return FALSE;
    lpPoint->x += x;
    lpPoint->y += y;
    return TRUE;
}

HMONITOR WINAPI MonitorFromPoint(tagPOINT pt, DWORD dwFlags)
{
    if (!EnsureVideo())
        return nullptr;
    int count = 0;
    SDL_DisplayID *const displays = SDL_GetDisplays(&count);
    SDL_DisplayID containing = 0;
    for (int index = 0; displays && index < count && !containing; ++index)
    {
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(displays[index], &bounds) && pt.x >= bounds.x && pt.y >= bounds.y &&
            pt.x < bounds.x + bounds.w && pt.y < bounds.y + bounds.h)
            containing = displays[index];
    }
    SDL_free(displays);
    if (containing)
        return ToMonitor(containing);
    if (dwFlags == MONITOR_DEFAULTTOPRIMARY)
        return ToMonitor(SDL_GetPrimaryDisplay());
    if (dwFlags == MONITOR_DEFAULTTONEAREST)
    {
        const SDL_Point point{pt.x, pt.y};
        return ToMonitor(SDL_GetDisplayForPoint(&point));
    }
    return nullptr;
}

BOOL WINAPI EnumDisplayMonitors(HDC hdc, const tagRECT *lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData)
{
    // The renderer enumerates every monitor of the desktop; clipping to a device
    // context has no SDL counterpart.
    if (hdc || lprcClip || !lpfnEnum || !EnsureVideo())
        return FALSE;
    int count = 0;
    SDL_DisplayID *const displays = SDL_GetDisplays(&count);
    if (!displays)
        return FALSE;
    BOOL completed = TRUE;
    for (int index = 0; index < count; ++index)
    {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(displays[index], &bounds))
            continue;
        tagRECT rect = ToRect(bounds);
        if (!lpfnEnum(ToMonitor(displays[index]), nullptr, &rect, dwData))
        {
            completed = FALSE;
            break;
        }
    }
    SDL_free(displays);
    return completed;
}

BOOL WINAPI GetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFO lpmi)
{
    if (!lpmi || lpmi->cbSize < sizeof(MONITORINFO) || !EnsureVideo())
        return FALSE;
    const SDL_DisplayID display = ToDisplay(hMonitor);
    if (!DisplayExists(display))
        return FALSE;
    SDL_Rect bounds{};
    SDL_Rect usable{};
    if (!SDL_GetDisplayBounds(display, &bounds))
        return FALSE;
    if (!SDL_GetDisplayUsableBounds(display, &usable))
        usable = bounds;
    lpmi->rcMonitor = ToRect(bounds);
    lpmi->rcWork = ToRect(usable);
    lpmi->dwFlags = display == SDL_GetPrimaryDisplay() ? MONITORINFOF_PRIMARY : 0u;
    return TRUE;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName)
{
    static char processModuleToken;
    return lpModuleName ? nullptr : reinterpret_cast<HMODULE>(&processModuleToken);
}

int WINAPI MessageBoxA(HWND, LPCSTR lpText, LPCSTR lpCaption, UINT)
{
    std::fprintf(stderr, "%s: %s\n", lpCaption ? lpCaption : "Message", lpText ? lpText : "");
    return IDOK;
}

HINSTANCE WINAPI ShellExecuteA(HWND, LPCSTR lpOperation, LPCSTR lpFile, LPCSTR, LPCSTR, int)
{
    std::fprintf(stderr, "win32_window: ShellExecuteA refused (%s %s): nothing is run on this platform\n",
                 lpOperation ? lpOperation : "open", lpFile ? lpFile : "");
    // SE_ERR_ACCESSDENIED: values of 32 and below are failures.
    return reinterpret_cast<HINSTANCE>(static_cast<std::uintptr_t>(5));
}
