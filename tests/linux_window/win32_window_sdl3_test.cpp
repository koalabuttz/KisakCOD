// Asset-free checks for src/linux/client/window/win32_window_sdl3.cpp, the SDL3
// implementation of the Win32 window and monitor calls r_init.cpp makes.
//
// It runs on SDL's dummy video driver: no display server, no GPU, no Vulkan.
// That driver has one 1024 x 768 display and cannot make Vulkan windows, so the
// monitor calls are checked against what SDL reports, and CreateWindowExA is
// checked to fail closed. The successful window path needs a Vulkan driver and
// is exercised by KisakCOD-device-probe on hardware.
#include <linux/d3d9_types.h>

#include <linux/client/window/sdl_api.h>
#include <linux/client/window/win32_window_sdl3.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char *what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

std::uintptr_t Value(HMONITOR monitor)
{
    return reinterpret_cast<std::uintptr_t>(monitor);
}

struct Visit
{
    std::vector<std::uintptr_t> monitors;
    std::vector<tagRECT> rects;
    int stopAfter = 0;
};

BOOL CALLBACK RecordMonitor(HMONITOR monitor, HDC hdc, tagRECT *rect, LPARAM data)
{
    Visit *const visit = reinterpret_cast<Visit *>(data);
    visit->monitors.push_back(Value(monitor));
    visit->rects.push_back(rect ? *rect : tagRECT{});
    Check(hdc == nullptr, "EnumDisplayMonitors passes no device context");
    return visit->stopAfter && int(visit->monitors.size()) >= visit->stopAfter ? FALSE : TRUE;
}

// The shape of r_init.cpp's R_ChooseMonitor/R_MonitorEnumCallback pair: a
// countdown in the first word and the found handle in the second, through a
// stdcall callback with the engine's parameter types and a C-style cast.
struct EnumMonitors
{
    int monitorIndex;
    HMONITOR__ *foundMonitor;
};

int __stdcall EngineShapedCallback(HMONITOR__ *monitorHandle, HDC__ *, tagRECT *, DWORD *userData)
{
    if (--*userData)
        return 1;
    userData[1] = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(monitorHandle));
    return 0;
}

bool SameRect(const tagRECT &rect, const SDL_Rect &bounds)
{
    return rect.left == bounds.x && rect.top == bounds.y && rect.right == bounds.x + bounds.w &&
           rect.bottom == bounds.y + bounds.h;
}

void CheckStyles()
{
    using kisak::win32_window::WindowFlagsForStyle;
    const std::uint64_t windowed = WindowFlagsForStyle(WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 0);
    Check(windowed == SDL_WINDOW_VULKAN, "a visible captioned window is a plain Vulkan window");
    const std::uint64_t borderless = WindowFlagsForStyle(WS_VISIBLE | WS_POPUP, 0);
    Check(borderless == (SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS), "WS_POPUP is borderless");
    const std::uint64_t fullscreen = WindowFlagsForStyle(WS_POPUP, WS_EX_TOPMOST);
    Check(fullscreen == (SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALWAYS_ON_TOP),
          "r_init.cpp's fullscreen styles: hidden until shown, borderless, on top");
    Check(!(fullscreen & SDL_WINDOW_FULLSCREEN), "fullscreen is the device's business, not a window style");
}

void CheckWithoutVideo()
{
    Check(GetActiveWindow() == nullptr, "no active window before any window exists");
    Check(!IsWindow(nullptr), "a null handle is not a window");
    Check(!DestroyWindow(nullptr), "destroying a null handle fails");
    int unrelated = 0;
    Check(!IsWindow(reinterpret_cast<HWND>(&unrelated)), "an unknown pointer is not a window");
    Check(!DestroyWindow(reinterpret_cast<HWND>(&unrelated)), "an unknown pointer is not destroyed");
    Check(!ShowWindow(reinterpret_cast<HWND>(&unrelated), SW_SHOW), "an unknown pointer is not shown");

    tagRECT rect{10, 20, 650, 500};
    Check(AdjustWindowRectEx(&rect, WS_VISIBLE | WS_CAPTION | WS_SYSMENU, FALSE, 0) == TRUE,
          "AdjustWindowRectEx accepts a rectangle");
    Check(rect.left == 10 && rect.top == 20 && rect.right == 650 && rect.bottom == 500,
          "window sizes are client sizes: the rectangle is unchanged");
    Check(!AdjustWindowRectEx(nullptr, 0, FALSE, 0), "AdjustWindowRectEx refuses a null rectangle");

    const HMODULE self = GetModuleHandleA(nullptr);
    Check(self != nullptr && self == GetModuleHandleA(nullptr), "the process module token is stable and non-null");
    Check(GetModuleHandleA("d3d9.dll") == nullptr, "named modules are not found");

    const HINSTANCE shell = ShellExecuteA(nullptr, "open", "Docs\\TechHelp\\Tech Help\\Information\\DirectX.htm",
                                          nullptr, nullptr, 3);
    Check(reinterpret_cast<std::uintptr_t>(shell) <= 32, "ShellExecuteA always fails");
    Check(MessageBoxA(nullptr, "fixture message", "win32_window_sdl3_test", MB_ICONERROR) == IDOK,
          "MessageBoxA reports IDOK without a dialog");
    Check(kisak::win32_window::OpenWindowCount() == 0, "no window was created");
}

void CheckMonitors()
{
    Check(SDL_WasInit(SDL_INIT_VIDEO) == 0, "SDL video is not started before a monitor call");
    const int width = GetSystemMetrics(SM_CXSCREEN);
    Check(SDL_WasInit(SDL_INIT_VIDEO) != 0, "the first monitor call starts SDL video");
    int count = 0;
    SDL_DisplayID *const displays = SDL_GetDisplays(&count);
    Check(displays != nullptr && count >= 1, "the dummy driver reports a display");
    if (!displays || count < 1)
        return;
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    SDL_Rect primaryBounds{};
    Check(SDL_GetDisplayBounds(primary, &primaryBounds), "the primary display has bounds");

    Visit all;
    Check(EnumDisplayMonitors(nullptr, nullptr, RecordMonitor, reinterpret_cast<LPARAM>(&all)) == TRUE,
          "EnumDisplayMonitors completes");
    Check(int(all.monitors.size()) == count, "EnumDisplayMonitors visits every SDL display");
    for (int index = 0; index < count && index < int(all.monitors.size()); ++index)
    {
        SDL_Rect bounds{};
        SDL_GetDisplayBounds(displays[index], &bounds);
        Check(all.monitors[index] == displays[index], "an HMONITOR is the SDL display id, in SDL's order");
        Check(SameRect(all.rects[index], bounds), "the monitor rectangle is the display bounds");
    }

    Visit first;
    first.stopAfter = 1;
    Check(EnumDisplayMonitors(nullptr, nullptr, RecordMonitor, reinterpret_cast<LPARAM>(&first)) == FALSE,
          "a callback that stops ends the enumeration with FALSE");
    Check(first.monitors.size() == 1, "nothing is visited after the callback stops");

    tagRECT clip{0, 0, 1, 1};
    int dc = 0;
    Visit none;
    Check(!EnumDisplayMonitors(reinterpret_cast<HDC>(&dc), nullptr, RecordMonitor, reinterpret_cast<LPARAM>(&none)),
          "a device context is refused");
    Check(!EnumDisplayMonitors(nullptr, &clip, RecordMonitor, reinterpret_cast<LPARAM>(&none)),
          "a clip rectangle is refused");
    Check(!EnumDisplayMonitors(nullptr, nullptr, nullptr, 0), "a missing callback is refused");
    Check(none.monitors.empty(), "refused enumerations call nothing");

    EnumMonitors engine{1, nullptr};
    // The engine's own cast, which GCC reports under -Wextra.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
    EnumDisplayMonitors(nullptr, nullptr, (MONITORENUMPROC)EngineShapedCallback, (LPARAM)&engine);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    Check(Value(engine.foundMonitor) == displays[0], "the engine's callback finds monitor 1, the first display");

    const tagPOINT inside{primaryBounds.x + 3, primaryBounds.y + 22};
    Check(Value(MonitorFromPoint(inside, MONITOR_DEFAULTTOPRIMARY)) == primary,
          "r_init.cpp's default window position is on the primary display");
    const tagPOINT outside{primaryBounds.x + 100000, primaryBounds.y + 100000};
    Check(MonitorFromPoint(outside, MONITOR_DEFAULTTONULL) == nullptr, "a point off every display has no monitor");
    Check(Value(MonitorFromPoint(outside, MONITOR_DEFAULTTOPRIMARY)) == primary,
          "MONITOR_DEFAULTTOPRIMARY falls back to the primary display");
    Check(MonitorFromPoint(outside, MONITOR_DEFAULTTONEAREST) != nullptr,
          "MONITOR_DEFAULTTONEAREST always finds a display");

    MONITORINFO info{};
    info.cbSize = 40;
    const HMONITOR primaryMonitor = MonitorFromPoint(inside, MONITOR_DEFAULTTOPRIMARY);
    Check(GetMonitorInfoA(primaryMonitor, &info) == TRUE, "GetMonitorInfoA accepts the engine's cbSize of 40");
    Check(SameRect(info.rcMonitor, primaryBounds), "rcMonitor is the display bounds");
    Check(info.rcWork.left >= info.rcMonitor.left && info.rcWork.right <= info.rcMonitor.right &&
              info.rcWork.top >= info.rcMonitor.top && info.rcWork.bottom <= info.rcMonitor.bottom,
          "rcWork lies inside rcMonitor");
    Check(info.dwFlags == MONITORINFOF_PRIMARY, "the primary display is flagged primary");
    MONITORINFO small{};
    small.cbSize = 39;
    Check(!GetMonitorInfoA(primaryMonitor, &small), "a short MONITORINFO is refused");
    Check(!GetMonitorInfoA(reinterpret_cast<HMONITOR>(std::uintptr_t(0x7fff0000u)), &info),
          "an unknown monitor is refused");
    Check(!GetMonitorInfoA(nullptr, &info), "a null monitor is refused");

    const SDL_DisplayMode *const mode = SDL_GetCurrentDisplayMode(primary);
    Check(mode && width == mode->w && GetSystemMetrics(SM_CYSCREEN) == mode->h,
          "SM_CXSCREEN and SM_CYSCREEN are the primary display's mode");
    Check(GetSystemMetrics(2) == 0, "other metrics are not reported");
    SDL_free(displays);
}

void CheckWindowsFailClosed()
{
    int parent = 0;
    Check(CreateWindowExA(0, "CoD4", "Call of Duty 4", WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 3, 22, 640, 480,
                          reinterpret_cast<HWND>(&parent), nullptr, GetModuleHandleA(nullptr), nullptr) == nullptr,
          "child windows are refused");
    Check(CreateWindowExA(0, "CoD4", "Call of Duty 4", WS_VISIBLE, 3, 22, 0, 480, nullptr, nullptr,
                          GetModuleHandleA(nullptr), nullptr) == nullptr,
          "an empty window is refused");
    // r_init.cpp's windowed call. The dummy driver has no Vulkan, so SDL cannot
    // give DXVK a window; the call must fail and leave nothing behind.
    const HWND window = CreateWindowExA(0, "CoD4", "Call of Duty 4", WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 3, 22,
                                        640, 480, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    Check(window == nullptr, "without a Vulkan-capable video driver there is no window");
    Check(kisak::win32_window::OpenWindowCount() == 0, "a failed creation registers nothing");
    Check(GetActiveWindow() == nullptr, "there is still no active window");
}

} // namespace

int main()
{
    // Before any Win32 call starts SDL video. The checks are written for the
    // dummy driver's one display, so it overrides SDL_VIDEO_DRIVER and the SDL2
    // spelling SDL_VIDEODRIVER, which a plain SDL_SetHint would defer to.
    if (!SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE))
    {
        std::printf("FAIL cannot select SDL's dummy video driver: %s\n", SDL_GetError());
        return 1;
    }
    CheckStyles();
    CheckWithoutVideo();
    CheckMonitors();
    const char *const driver = SDL_GetCurrentVideoDriver();
    Check(driver && !std::strcmp(driver, "dummy"), "the checks ran on the dummy video driver");
    CheckWindowsFailClosed();
    std::printf("%s win32 window layer: %d checks, %d failures (video driver %s)\n",
                failures ? "FAIL" : "PASS", checks, failures, driver ? driver : "none");
    return failures ? 1 : 0;
}
