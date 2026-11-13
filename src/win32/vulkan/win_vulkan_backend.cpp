// win_vulkan_backend.cpp -- r_backend vulkan on Windows: the renderer's
// Direct3D 9 calls drawn through the Vulkan back end (the D3D9 front,
// src/gfx_vk/d3d9, on the Legacy adapter) instead of d3d9.dll.
//
// Built into KisakCOD-mp only with KISAK_BUILD_VULKAN_BACKEND
// (scripts/vulkan_probe/vulkan_backend_windows.cmake), which also defines
// KISAK_VULKAN_BACKEND for the game, so R_HW_Direct3DCreate9
// (src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp) asks here first. Every later D3D9 call
// lands on whichever IDirect3D9 it returned, so nothing else in the renderer
// changes.
//
// The Linux and macOS host (src/linux/client/vulkan/vulkan_backend.cpp) is SDL3;
// this one is plain Win32: the HWND is the game's window, the Vulkan loader
// (vulkan-1.dll) is opened at run time rather than imported, so the game still
// starts on a machine without one, and display modes and the monitor are the
// primary display's. The dvars are the same on every platform. See
// docs/vulkan-d3d9-front.md.

#if !defined(KISAK_VULKAN_BACKEND)
#error "win_vulkan_backend.cpp belongs to KisakCOD-mp built with KISAK_BUILD_VULKAN_BACKEND"
#endif

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>

#include <windows.h>

#include <gfx_vk/d3d9/vk_d3d9.h>
#include <gfx_vk/modern/vk_modern.h>
// After d3d9.h (above), so IDirect3DDevice9 is the SDK's.
#include <gfx_d3d/rhi/r_rhi_modern.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

IDirect3D9 *Kisak_VulkanDirect3DCreate9();
int Kisak_VulkanBackendSelected();
void Kisak_VulkanWindowDestroying(HWND window);
void Kisak_VulkanAppActivated(HWND window, bool active);
void Kisak_VulkanWindowSized(HWND window);
unsigned int Kisak_VulkanLogStats(bool reset);

namespace
{
namespace front = gfx_vk::d3d9;

const char *s_backendNames[] = {"d3d9", "vulkan", nullptr};
const char *s_validationNames[] = {"off", "auto", "require", nullptr};
enum : int
{
    BACKEND_D3D9 = 0,
    BACKEND_VULKAN = 1,
};

const dvar_s *r_backend;
const dvar_s *r_vkValidation;
const dvar_s *r_vkDevice;
const dvar_s *r_vkShaderCache;
// Read at every Present (HostTuning), so a change shows in the next frame.
const char *s_gpuFeedNames[] = {"default", "off", "on", "always", nullptr};
const dvar_s *r_vkGpuFeed;
const dvar_s *r_vkGpuFeedDraws;
const dvar_s *r_vkGpuFeedIdleDraws;
const dvar_s *r_vkKeepDrawState;
const char *s_visualsNames[] = {"legacy", "modern", nullptr};
enum : int
{
    VISUALS_LEGACY = 0,
    VISUALS_MODERN = 1,
};
const dvar_s *r_visuals;

const char *const s_instanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
};

struct DisplayMode
{
    uint32_t width, height, refreshRate;
};

// The host's state. The front calls the host under its own lock, from whichever
// thread is calling D3D9.
struct HostState
{
    HMODULE loader = nullptr; // vulkan-1.dll, kept for the life of the process
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    std::vector<DisplayMode> modes;
    std::string shaderCachePath;   // what Host::shaderCachePath points at
    std::string pipelineCachePath; // what Host::pipelineCachePath points at
    // Fullscreen (HostFullscreen): the device window, the mode it asked for,
    // and whether the primary display is now at a mode of ours rather than
    // the desktop's. The front calls under its lock, WM_ACTIVATEAPP comes on
    // the window's thread: both take this one (recursive, as moving the
    // window on its own thread runs its window procedure).
    std::recursive_mutex fullscreenLock;
    HWND fullscreenWindow = nullptr;
    front::FullscreenMode fullscreenMode{};
    bool displayChanged = false;
};
// Not s_host: winsock.h defines that name as a macro.
HostState s_vkHost;

// ---- The Vulkan loader -----------------------------------------------------------

// Opens vulkan-1.dll once, from the system directory only (never the game's
// folder or the current directory), and keeps it open: a later device reuses it.
bool LoadVulkanLoader()
{
    if (s_vkHost.getInstanceProcAddr)
        return true;
    if (!s_vkHost.loader)
        s_vkHost.loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s_vkHost.loader)
    {
        Com_Printf(CON_CHANNEL_GFX,
                   "Vulkan: vulkan-1.dll did not load (error %lu); install a GPU driver with Vulkan support\n",
                   GetLastError());
        return false;
    }
    s_vkHost.getInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(s_vkHost.loader, "vkGetInstanceProcAddr"));
    if (!s_vkHost.getInstanceProcAddr)
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: vulkan-1.dll has no vkGetInstanceProcAddr\n");
        return false;
    }
    return true;
}

// ---- The window system --------------------------------------------------------

bool CreateSurface(void *user, VkInstance instance, VkSurfaceKHR *surface)
{
    const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        s_vkHost.getInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (!create)
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: the instance has no vkCreateWin32SurfaceKHR\n");
        return false;
    }
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(user);
    const VkResult result = create(instance, &info, nullptr, surface);
    if (result == VK_SUCCESS)
        return true;
    Com_Printf(CON_CHANNEL_GFX, "Vulkan: vkCreateWin32SurfaceKHR failed (%d)\n", static_cast<int>(result));
    return false;
}

void GetPixelSize(void *user, uint32_t *width, uint32_t *height)
{
    const HWND window = static_cast<HWND>(user);
    RECT client{};
    if (IsIconic(window) || !GetClientRect(window, &client))
        client = RECT{};
    const LONG w = client.right - client.left;
    const LONG h = client.bottom - client.top;
    *width = uint32_t(w > 0 ? w : 0);
    *height = uint32_t(h > 0 ? h : 0);
}

bool HostWindowSystem(void *, front::D3D9Window window, gfx_vk::WindowSystem &out)
{
    if (!window || !IsWindow(window))
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: the device window is not a window\n");
        return false;
    }
    if (!LoadVulkanLoader())
        return false;
    out = gfx_vk::WindowSystem{};
    out.getInstanceProcAddr = s_vkHost.getInstanceProcAddr;
    out.instanceExtensions = s_instanceExtensions;
    out.instanceExtensionCount = uint32_t(sizeof(s_instanceExtensions) / sizeof(s_instanceExtensions[0]));
    out.user = window;
    out.createSurface = CreateSurface;
    out.getPixelSize = GetPixelSize;
    return true;
}

// ---- Display modes and the monitor ----------------------------------------------

uint32_t RefreshRate(const DEVMODEW &mode)
{
    // 0 and 1 mean "the hardware's default rate".
    return mode.dmDisplayFrequency > 1 ? uint32_t(mode.dmDisplayFrequency) : 60u;
}

// The primary display's 32-bit modes, smallest first and without repeats, as
// D3D9 lists an adapter's D3DFMT_X8R8G8B8 modes. A display that lists none
// offers its current mode, so the renderer has a mode to choose.
void LoadModes()
{
    s_vkHost.modes.clear();
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(nullptr, index, &mode); ++index)
    {
        if (mode.dmBitsPerPel == 32 && mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0)
            s_vkHost.modes.push_back({uint32_t(mode.dmPelsWidth), uint32_t(mode.dmPelsHeight), RefreshRate(mode)});
        mode = DEVMODEW{};
        mode.dmSize = sizeof(mode);
    }
    if (s_vkHost.modes.empty() && EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
        s_vkHost.modes.push_back({uint32_t(mode.dmPelsWidth), uint32_t(mode.dmPelsHeight), RefreshRate(mode)});
    std::sort(s_vkHost.modes.begin(), s_vkHost.modes.end(), [](const DisplayMode &a, const DisplayMode &b) {
        if (a.width != b.width)
            return a.width < b.width;
        if (a.height != b.height)
            return a.height < b.height;
        return a.refreshRate < b.refreshRate;
    });
    s_vkHost.modes.erase(std::unique(s_vkHost.modes.begin(), s_vkHost.modes.end(),
                                   [](const DisplayMode &a, const DisplayMode &b) {
                                       return a.width == b.width && a.height == b.height &&
                                              a.refreshRate == b.refreshRate;
                                   }),
                       s_vkHost.modes.end());
}

uint32_t HostModeCount(void *)
{
    LoadModes();
    return uint32_t(s_vkHost.modes.size());
}

bool HostMode(void *, uint32_t index, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    if (index >= s_vkHost.modes.size())
        return false;
    width = s_vkHost.modes[index].width;
    height = s_vkHost.modes[index].height;
    refreshRate = s_vkHost.modes[index].refreshRate;
    return true;
}

bool HostCurrentMode(void *, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
        return false;
    width = uint32_t(mode.dmPelsWidth);
    height = uint32_t(mode.dmPelsHeight);
    refreshRate = RefreshRate(mode);
    return true;
}

HMONITOR HostMonitor(void *)
{
    // The one adapter the front offers is the primary display.
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

// ---- Fullscreen ------------------------------------------------------------------

// The primary display at the mode, unless it is there already. False when
// Windows refuses it.
bool SetDisplayMode(const front::FullscreenMode &mode)
{
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    const bool known = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current) != 0;
    const DWORD width = mode.width ? DWORD(mode.width) : current.dmPelsWidth;
    const DWORD height = mode.height ? DWORD(mode.height) : current.dmPelsHeight;
    if (known && current.dmPelsWidth == width && current.dmPelsHeight == height
        && (!mode.refreshRate || RefreshRate(current) == mode.refreshRate))
        return true;
    DEVMODEW wanted{};
    wanted.dmSize = sizeof(wanted);
    wanted.dmPelsWidth = width;
    wanted.dmPelsHeight = height;
    wanted.dmBitsPerPel = 32;
    wanted.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (mode.refreshRate)
    {
        wanted.dmDisplayFrequency = mode.refreshRate;
        wanted.dmFields |= DM_DISPLAYFREQUENCY;
    }
    const LONG result = ChangeDisplaySettingsExW(nullptr, &wanted, nullptr, CDS_FULLSCREEN, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL)
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: the display refused %lux%lu at %u Hz (%ld)\n",
                   static_cast<unsigned long>(width), static_cast<unsigned long>(height), mode.refreshRate, result);
        return false;
    }
    s_vkHost.displayChanged = true;
    return true;
}

void RestoreDisplayMode()
{
    if (!s_vkHost.displayChanged)
        return;
    s_vkHost.displayChanged = false;
    // Null: the mode saved in the registry, the desktop's.
    const LONG result = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL)
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: the desktop's display mode did not come back (%ld)\n", result);
}

// The window over the whole primary display, as D3D9 places a fullscreen
// device's window. From another thread than the window's, without waiting
// for it (that thread may be waiting for this one).
void CoverMonitor(HWND window)
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!IsWindow(window) || !GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info))
        return;
    const RECT &rect = info.rcMonitor;
    if (IsIconic(window))
    {
        // Minimised (the player is coming back to it): the size it comes
        // back at. Windows set that to the desktop's whole display when the
        // desktop's mode came back (PC run 18b), which on our smaller mode
        // showed a corner of the picture. The placement is in workspace
        // coordinates, which start at the work area rather than the display.
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(window, &placement))
        {
            const LONG x = rect.left - (info.rcWork.left - info.rcMonitor.left);
            const LONG y = rect.top - (info.rcWork.top - info.rcMonitor.top);
            placement.rcNormalPosition = RECT{x, y, x + (rect.right - rect.left), y + (rect.bottom - rect.top)};
            SetWindowPlacement(window, &placement);
        }
        return;
    }
    UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
        flags |= SWP_ASYNCWINDOWPOS;
    SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags);
}

bool HostFullscreen(void *, front::D3D9Window window, const front::FullscreenMode *mode)
{
    const std::lock_guard<std::recursive_mutex> lock(s_vkHost.fullscreenLock);
    if (!mode)
    {
        s_vkHost.fullscreenWindow = nullptr;
        RestoreDisplayMode();
        return true;
    }
    s_vkHost.fullscreenWindow = window;
    s_vkHost.fullscreenMode = *mode;
    const bool set = SetDisplayMode(*mode);
    CoverMonitor(window);
    return set;
}

// The window's client area on its monitor, for windowed screenshots (the
// renderer measures it the same way, with MonitorFromWindow).
bool HostWindowOrigin(void *, front::D3D9Window window, int32_t &x, int32_t &y)
{
    POINT origin{0, 0};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!ClientToScreen(window, &origin)
        || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info))
        return false;
    x = origin.x - info.rcMonitor.left;
    y = origin.y - info.rcMonitor.top;
    return true;
}

void HostLog(void *, const char *line)
{
    Com_Printf(CON_CHANNEL_GFX, "Vulkan: %s\n", line);
}

void HostCoreDestroyed(void *, const gfx_vk::CoreStats &stats)
{
    Com_Printf(CON_CHANNEL_GFX,
               "Vulkan: device destroyed after %llu frames (%llu skipped), %u validation errors, %u warnings\n",
               static_cast<unsigned long long>(stats.framesPresented),
               static_cast<unsigned long long>(stats.framesSkipped), stats.validationErrors,
               stats.validationWarnings);
}

// Where the caches live: <fs_homepath>\\vulkan, holding the Legacy adapter's
// shaders.bin (translated shaders) and the core's pipelines.bin (the Vulkan
// pipeline cache). Empty (no files) when r_vkShaderCache is off or there is no
// home path.
std::string CacheDirectory()
{
    if (!r_vkShaderCache->current.enabled)
        return {};
    const char *home = Dvar_GetString("fs_homepath");
    if (!home || !*home)
        return {};
    const std::string directory = std::string(home) + "\\vulkan";
    if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: no shader or pipeline cache: cannot create %s\n", directory.c_str());
        return {};
    }
    return directory;
}

// The dvars the front takes up while a device exists (front::Tuning).
void HostTuning(void *, front::Tuning &out)
{
    out.keepDrawState = r_vkKeepDrawState->current.enabled;
    out.gpuFeed = r_vkGpuFeed->current.integer - 1;
    // In parentheses: <windows.h> defines max as a macro here.
    out.gpuFeedDraws = static_cast<uint32_t>((std::max)(0, r_vkGpuFeedDraws->current.integer));
    out.gpuFeedIdleDraws = static_cast<uint32_t>((std::max)(0, r_vkGpuFeedIdleDraws->current.integer));
}

gfx_vk::Validation ValidationSetting()
{
    switch (r_vkValidation->current.integer)
    {
    case 1:
        return gfx_vk::Validation::IfFound;
    case 2:
        return gfx_vk::Validation::Required;
    default:
        return gfx_vk::Validation::Off;
    }
}

// The same dvars as on Linux and macOS (latched and archived; harmless to call again).
void RegisterDvars()
{
    r_backend = Dvar_RegisterEnum("r_backend", s_backendNames, BACKEND_D3D9, DVAR_ARCHIVE | DVAR_LATCH,
                                  "Graphics back end: d3d9 (the system's Direct3D 9) or vulkan "
                                  "(KisakCOD's own Vulkan renderer)");
    r_vkValidation = Dvar_RegisterEnum("r_vkValidation", s_validationNames, 0, DVAR_ARCHIVE | DVAR_LATCH,
                                       "Vulkan validation layer: off, auto (when installed) or require");
    r_vkDevice = Dvar_RegisterInt("r_vkDevice", -1, uint32_t(-1), 63, DVAR_ARCHIVE | DVAR_LATCH,
                                  "Vulkan device: -1 picks one, N the Nth device the Vulkan driver lists");
    r_vkShaderCache = Dvar_RegisterBool("r_vkShaderCache", true, DVAR_ARCHIVE | DVAR_LATCH,
                                        "Keep translated shaders and Vulkan pipelines in fs_homepath/vulkan "
                                        "(shaders.bin, pipelines.bin) between runs");
    // Measuring switches: not saved, and in effect from the next frame, so
    // one launch can compare settings.
    r_vkGpuFeed = Dvar_RegisterEnum("r_vkGpuFeed", s_gpuFeedNames, 0, DVAR_NOFLAG,
                                    "Vulkan: hand the GPU a frame in parts while it is drawn: default (by the "
                                    "kind of GPU), off, on, or always (after every draw, for testing)");
    r_vkGpuFeedDraws = Dvar_RegisterInt("r_vkGpuFeedDraws", 0, 0, 65535, DVAR_NOFLAG,
                                        "Vulkan: draws to each part of a frame handed to the GPU; 0 is the "
                                        "default, 128");
    r_vkGpuFeedIdleDraws = Dvar_RegisterInt("r_vkGpuFeedIdleDraws", 0, 0, 65535, DVAR_NOFLAG,
                                            "Vulkan: draws to a part when the GPU has nothing left (the next "
                                            "such part of the frame takes twice as many); 0 for no such parts");
    r_vkKeepDrawState = Dvar_RegisterBool("r_vkKeepDrawState", true, DVAR_NOFLAG,
                                          "Vulkan: a draw takes up the pipeline, constants and binds the draw "
                                          "before it left; off makes every draw set them all again");
    r_visuals = Dvar_RegisterEnum("r_visuals", s_visualsNames, VISUALS_LEGACY, DVAR_ARCHIVE | DVAR_LATCH,
                                  "Visuals with r_backend vulkan: legacy (the game as it was made) or modern "
                                  "(experimental; so far temporal anti-aliasing)");
}

// ---- The Modern path (r_visuals modern; docs/vulkan-modern.md) ------------------------

// Set when the front refused a scene (a multisampled back buffer, say): the
// scenes are not jittered any more, since nothing would take the jitter out.
// Read on the front end's thread, written on the render thread.
std::atomic<bool> s_modernRefused{false};

void ModernJitterPixels(void *, uint32_t frame, float *x, float *y)
{
    if (s_modernRefused.load(std::memory_order_relaxed))
    {
        *x = *y = 0.0f;
        return;
    }
    gfx_vk::modern::JitterPixels(frame, *x, *y);
}

// The front's view of the engine's scene.
front::ModernSceneView ModernViewOf(const GfxModernScene *scene)
{
    front::ModernSceneView view{};
    std::memcpy(view.view, scene->view, sizeof(view.view));
    std::memcpy(view.projection, scene->projection, sizeof(view.projection));
    view.x = uint32_t((std::max)(0, scene->x));
    view.y = uint32_t((std::max)(0, scene->y));
    view.width = uint32_t((std::max)(0, scene->width));
    view.height = uint32_t((std::max)(0, scene->height));
    view.cut = scene->cut;
    view.glow.on = scene->glow;
    view.glow.radius = scene->glowRadius;
    view.glow.intensity = scene->glowIntensity;
    view.glow.cutoff = scene->glowCutoff;
    view.glow.desaturation = scene->glowDesaturation;
    return view;
}

// Whether the scene was finished before its effects (ModernSceneLit): its
// sceneDone then has nothing to do. Render thread only.
bool s_modernSceneFinished = false;

void ModernFinish(GfxModernScene *scene, front::ModernResult result, bool glowDrawn)
{
    if (glowDrawn)
        scene->glowDrawn = true;
    if (result == front::ModernResult::Refused)
        s_modernRefused.store(true, std::memory_order_relaxed);
}

// Effects on the display image (unless KISAK_VK_MODERN_EFFECTS=linear): the
// front finishes the scene here.
void ModernSceneLit(void *, IDirect3DDevice9 *device, GfxModernScene *scene)
{
    if (s_modernRefused.load(std::memory_order_relaxed))
        return;
    bool glowDrawn = false;
    const front::ModernResult result = front::ModernSceneLit(device, ModernViewOf(scene), &glowDrawn);
    if (result == front::ModernResult::Skipped)
        return;
    s_modernSceneFinished = true;
    ModernFinish(scene, result, glowDrawn);
}

void ModernSceneDone(void *, IDirect3DDevice9 *device, GfxModernScene *scene)
{
    if (s_modernSceneFinished)
    {
        s_modernSceneFinished = false;
        return;
    }
    if (s_modernRefused.load(std::memory_order_relaxed))
        return;
    bool glowDrawn = false;
    const front::ModernResult result = front::ModernSceneDone(device, ModernViewOf(scene), &glowDrawn);
    ModernFinish(scene, result, glowDrawn);
}

// Linear light (KISAK_VK_MODERN_LINEAR): a refusal here (logged once by the
// front) leaves the scene in the back buffer, and the anti-aliasing runs on.
void ModernSceneBegin(void *, IDirect3DDevice9 *device)
{
    s_modernSceneFinished = false;
    if (!s_modernRefused.load(std::memory_order_relaxed))
        front::ModernSceneBegin(device);
}

const GfxModernHooks s_modernHooks = {nullptr, ModernJitterPixels, ModernSceneDone, ModernSceneBegin, ModernSceneLit};
} // namespace

int Kisak_VulkanBackendSelected()
{
    RegisterDvars();
    if (r_backend->current.integer == BACKEND_VULKAN)
        return 1;
    // The Modern path's hooks belong to the Vulkan back end.
    R_SetModernHooks(nullptr);
    return 0;
}

IDirect3D9 *Kisak_VulkanDirect3DCreate9()
{
    RegisterDvars();
    // Like d3d9.dll's Direct3DCreate9, fail here when there is no Vulkan at
    // all, so the renderer reports that Direct3D failed to initialize.
    if (!LoadVulkanLoader())
        return nullptr;
    front::Host host;
    host.user = &s_vkHost;
    host.windowSystem = HostWindowSystem;
    host.modeCount = HostModeCount;
    host.mode = HostMode;
    host.currentMode = HostCurrentMode;
    host.monitor = HostMonitor;
    host.windowOrigin = HostWindowOrigin;
    host.fullscreen = HostFullscreen;
    host.log = HostLog;
    host.validation = ValidationSetting();
    host.deviceIndex = r_vkDevice->current.integer;
    host.coreDestroyed = HostCoreDestroyed;
    host.tuning = HostTuning;
    const std::string cacheDirectory = CacheDirectory();
    s_vkHost.shaderCachePath = cacheDirectory.empty() ? std::string() : cacheDirectory + "\\shaders.bin";
    s_vkHost.pipelineCachePath = cacheDirectory.empty() ? std::string() : cacheDirectory + "\\pipelines.bin";
    host.shaderCachePath = s_vkHost.shaderCachePath.empty() ? nullptr : s_vkHost.shaderCachePath.c_str();
    host.pipelineCachePath = s_vkHost.pipelineCachePath.empty() ? nullptr : s_vkHost.pipelineCachePath.c_str();
    Com_Printf(CON_CHANNEL_GFX, "Graphics back end: Vulkan (validation %s, device %d, shader cache %s, visuals %s)\n",
               s_validationNames[r_vkValidation->current.integer], r_vkDevice->current.integer,
               s_vkHost.shaderCachePath.empty() ? "off" : s_vkHost.shaderCachePath.c_str(),
               s_visualsNames[r_visuals->current.integer]);
    s_modernRefused.store(false, std::memory_order_relaxed);
    R_SetModernHooks(r_visuals->current.integer == VISUALS_MODERN ? &s_modernHooks : nullptr);
    return front::CreateDirect3D9(host);
}

// WM_DESTROY: a device made for the window lets go of its Vulkan surface while
// the window still exists (the engine destroys its window before it releases
// the device, and a surface must not outlive its window).
void Kisak_VulkanWindowDestroying(HWND window)
{
    front::WindowDestroying(window);
}

// WM_ACTIVATEAPP: D3D9 gives a fullscreen device's display mode back to the
// desktop and minimizes the window when the player switches away, and sets
// it again on the way back. The front's devices are fullscreen through
// HostFullscreen, so this does the same.
void Kisak_VulkanAppActivated(HWND window, bool active)
{
    const std::lock_guard<std::recursive_mutex> lock(s_vkHost.fullscreenLock);
    if (!s_vkHost.fullscreenWindow || window != s_vkHost.fullscreenWindow)
        return;
    if (active)
    {
        SetDisplayMode(s_vkHost.fullscreenMode);
        CoverMonitor(window);
        RECT rect{};
        GetWindowRect(window, &rect);
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: fullscreen game active again: window (%ld, %ld, %ld, %ld)%s\n",
                   rect.left, rect.top, rect.right, rect.bottom, IsIconic(window) ? ", minimised" : "");
        return;
    }
    if (s_vkHost.displayChanged)
    {
        // Minimised first, then the desktop's mode, as DXVK does. The other
        // way round (PC run 18) left the window at the desktop's full size,
        // topmost and drawing: Windows resizes a window that covered the
        // display when the mode changes back. Posted again if it still shows.
        if (IsWindowVisible(window))
            ShowWindow(window, SW_MINIMIZE);
        RestoreDisplayMode();
        if (IsWindowVisible(window) && !IsIconic(window))
            ShowWindowAsync(window, SW_MINIMIZE);
    }
}

// WM_SIZE of the game's window: a fullscreen window that is resized to
// anything but the display's size while the display is at our mode is moved
// back over the display. PC runs 18b and 18c: restoring the minimised window
// activates it while it is still minimised, so the restore size the host
// sets then comes too late, and the window came back at the desktop's full
// size (2560x1080) on the game's 1280x720 mode.
void Kisak_VulkanWindowSized(HWND window)
{
    std::unique_lock<std::recursive_mutex> lock(s_vkHost.fullscreenLock, std::try_to_lock);
    if (!lock.owns_lock() || window != s_vkHost.fullscreenWindow || !s_vkHost.displayChanged || IsIconic(window))
        return;
    RECT client{};
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!GetClientRect(window, &client) || !EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current))
        return;
    const LONG width = client.right - client.left, height = client.bottom - client.top;
    if (width == LONG(current.dmPelsWidth) && height == LONG(current.dmPelsHeight))
        return;
    Com_Printf(CON_CHANNEL_GFX, "Vulkan: fullscreen window sized %ldx%ld on a %lux%lu display: moved back over it\n",
               width, height, static_cast<unsigned long>(current.dmPelsWidth),
               static_cast<unsigned long>(current.dmPelsHeight));
    CoverMonitor(window);
}

// For the renderer's r_vkStats command (src/gfx_d3d/r_cmds.cpp): logs the
// device's frame statistics since it was made or since they were last started
// again, and with reset starts them again. Returns how many devices there
// were: 0 with r_backend d3d9.
unsigned int Kisak_VulkanLogStats(bool reset)
{
    return front::LogDeviceStats(reset);
}
