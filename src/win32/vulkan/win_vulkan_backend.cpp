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
#include <string>
#include <vector>

IDirect3D9 *Kisak_VulkanDirect3DCreate9();
int Kisak_VulkanBackendSelected();
void Kisak_VulkanWindowDestroying(HWND window);
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

void ModernSceneDone(void *, IDirect3DDevice9 *device, const GfxModernScene *scene)
{
    if (s_modernRefused.load(std::memory_order_relaxed))
        return;
    front::ModernSceneView view{};
    std::memcpy(view.view, scene->view, sizeof(view.view));
    std::memcpy(view.projection, scene->projection, sizeof(view.projection));
    view.x = uint32_t((std::max)(0, scene->x));
    view.y = uint32_t((std::max)(0, scene->y));
    view.width = uint32_t((std::max)(0, scene->width));
    view.height = uint32_t((std::max)(0, scene->height));
    view.cut = scene->cut;
    if (front::ModernSceneDone(device, view) == front::ModernResult::Refused)
        s_modernRefused.store(true, std::memory_order_relaxed);
}

const GfxModernHooks s_modernHooks = {nullptr, ModernJitterPixels, ModernSceneDone};
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

// For the renderer's r_vkStats command (src/gfx_d3d/r_cmds.cpp): logs the
// device's frame statistics since it was made or since they were last started
// again, and with reset starts them again. Returns how many devices there
// were: 0 with r_backend d3d9.
unsigned int Kisak_VulkanLogStats(bool reset)
{
    return front::LogDeviceStats(reset);
}
