// vulkan_backend.cpp -- r_backend vulkan on Linux and macOS: the D3D9 front of
// the Vulkan back end with an SDL3 host. See vulkan_backend.h.

#if !defined(KISAK_LINUX_NATIVE_D3D9)
#error "vulkan_backend.cpp uses the renderer's D3D9 declarations: build it through kisak_linux_native_d3d9"
#endif
#if !defined(KISAK_VULKAN_BACKEND)
#error "vulkan_backend.cpp belongs to programs built with KISAK_BUILD_VULKAN_BACKEND"
#endif

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>

#include <gfx_vk/d3d9/vk_d3d9.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/window/win32_window_sdl3.h>

#if KISAK_LINUX_SDL_MAJOR != 3
#error "the Vulkan back end's host is SDL3 (KISAK_LINUX_SDL_MAJOR=3)"
#endif
#include <SDL3/SDL_vulkan.h>

#include "vulkan_backend.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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

struct DisplayMode
{
    uint32_t width, height, refreshRate;
};

// The host's state. The front calls the host under its own lock, from whichever
// thread is calling D3D9; the teardown statistics are also read by the program,
// so they have a lock of their own.
struct HostState
{
    SDL_Window *window = nullptr; // the window CreateDevice was last given
    std::vector<DisplayMode> modes;
    std::string shaderCachePath; // what Host::shaderCachePath points at
    std::mutex teardownLock;
    bool teardownSeen = false;
    KisakVulkanTeardown teardown{};
};
HostState s_host;

SDL_DisplayID Display()
{
    return SDL_GetPrimaryDisplay();
}

uint32_t RefreshRate(const SDL_DisplayMode &mode)
{
    return mode.refresh_rate > 0.0f ? uint32_t(std::lround(mode.refresh_rate)) : 60u;
}

// SDL's fullscreen modes of the primary display, smallest first and without
// repeats, as D3D9 lists an adapter's modes. A display that lists none (SDL's
// offscreen driver) offers its desktop mode, so the renderer has a mode to
// choose; R_EnumDisplayModes refuses an adapter with none.
void LoadModes()
{
    s_host.modes.clear();
    const SDL_DisplayID display = Display();
    int count = 0;
    SDL_DisplayMode **const modes = display ? SDL_GetFullscreenDisplayModes(display, &count) : nullptr;
    for (int i = 0; modes && i < count; ++i)
    {
        if (modes[i] && modes[i]->w > 0 && modes[i]->h > 0)
            s_host.modes.push_back({uint32_t(modes[i]->w), uint32_t(modes[i]->h), RefreshRate(*modes[i])});
    }
    SDL_free(modes);
    if (s_host.modes.empty() && display)
    {
        if (const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display))
            s_host.modes.push_back({uint32_t(desktop->w), uint32_t(desktop->h), RefreshRate(*desktop)});
    }
    std::sort(s_host.modes.begin(), s_host.modes.end(), [](const DisplayMode &a, const DisplayMode &b) {
        if (a.width != b.width)
            return a.width < b.width;
        if (a.height != b.height)
            return a.height < b.height;
        return a.refreshRate < b.refreshRate;
    });
    s_host.modes.erase(std::unique(s_host.modes.begin(), s_host.modes.end(),
                                   [](const DisplayMode &a, const DisplayMode &b) {
                                       return a.width == b.width && a.height == b.height &&
                                              a.refreshRate == b.refreshRate;
                                   }),
                       s_host.modes.end());
}

// ---- The window system --------------------------------------------------------

bool CreateSurface(void *user, VkInstance instance, VkSurfaceKHR *surface)
{
    SDL_Window *const window = static_cast<SDL_Window *>(user);
    if (SDL_Vulkan_CreateSurface(window, instance, nullptr, surface))
        return true;
    Com_Printf(CON_CHANNEL_GFX, "Vulkan: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
    return false;
}

void GetPixelSize(void *user, uint32_t *width, uint32_t *height)
{
    SDL_Window *const window = static_cast<SDL_Window *>(user);
    int w = 0;
    int h = 0;
    if (!SDL_GetWindowSizeInPixels(window, &w, &h) || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
        w = h = 0;
    *width = uint32_t(w > 0 ? w : 0);
    *height = uint32_t(h > 0 ? h : 0);
}

bool HostWindowSystem(void *, front::D3D9Window handle, gfx_vk::WindowSystem &out)
{
    // The Win32 layer's HWND is the SDL_Window itself.
    SDL_Window *const window = reinterpret_cast<SDL_Window *>(handle);
    if (!window || !SDL_GetWindowID(window))
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: the device window is not an SDL window\n");
        return false;
    }
    // SDL loads the Vulkan loader when it creates a Vulkan window; the Win32
    // layer makes every window one.
    const auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    Uint32 extensionCount = 0;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!getInstanceProcAddr || !extensions)
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: no Vulkan loader for the window: %s\n", SDL_GetError());
        return false;
    }
    s_host.window = window;
    out = gfx_vk::WindowSystem{};
    out.getInstanceProcAddr = getInstanceProcAddr;
    out.instanceExtensions = extensions;
    out.instanceExtensionCount = extensionCount;
    out.user = window;
    out.createSurface = CreateSurface;
    out.getPixelSize = GetPixelSize;
    return true;
}

// ---- Display modes and the monitor ----------------------------------------------

uint32_t HostModeCount(void *)
{
    LoadModes();
    return uint32_t(s_host.modes.size());
}

bool HostMode(void *, uint32_t index, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    if (index >= s_host.modes.size())
        return false;
    width = s_host.modes[index].width;
    height = s_host.modes[index].height;
    refreshRate = s_host.modes[index].refreshRate;
    return true;
}

bool HostCurrentMode(void *, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    const SDL_DisplayID display = Display();
    const SDL_DisplayMode *const mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
    if (!mode)
        return false;
    width = uint32_t(mode->w);
    height = uint32_t(mode->h);
    refreshRate = RefreshRate(*mode);
    return true;
}

HMONITOR HostMonitor(void *)
{
    // The Win32 layer's HMONITOR is the SDL_DisplayID (MonitorFromWindow).
    return reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(Display()));
}

// The window's client area on its display, for windowed screenshots.
bool HostWindowOrigin(void *, front::D3D9Window handle, int32_t &x, int32_t &y)
{
    SDL_Window *const window = reinterpret_cast<SDL_Window *>(handle);
    int windowX = 0, windowY = 0;
    SDL_Rect bounds{};
    const SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : 0;
    if (!display || !SDL_GetWindowPosition(window, &windowX, &windowY) || !SDL_GetDisplayBounds(display, &bounds))
        return false;
    x = windowX - bounds.x;
    y = windowY - bounds.y;
    return true;
}

void HostLog(void *, const char *line)
{
    Com_Printf(CON_CHANNEL_GFX, "Vulkan: %s\n", line);
}

void HostCoreDestroyed(void *, const gfx_vk::CoreStats &stats)
{
    {
        std::lock_guard<std::mutex> lock(s_host.teardownLock);
        s_host.teardownSeen = true;
        s_host.teardown.framesPresented = stats.framesPresented;
        s_host.teardown.framesSkipped = stats.framesSkipped;
        s_host.teardown.validationErrors = stats.validationErrors;
        s_host.teardown.validationWarnings = stats.validationWarnings;
    }
    Com_Printf(CON_CHANNEL_GFX,
               "Vulkan: device destroyed after %llu frames (%llu skipped), %u validation errors, %u warnings\n",
               static_cast<unsigned long long>(stats.framesPresented),
               static_cast<unsigned long long>(stats.framesSkipped), stats.validationErrors,
               stats.validationWarnings);
}

// WM_DESTROY of any window: a device made for it lets go of its Vulkan surface
// while the window still exists (the engine destroys its windows before it
// releases the device).
void HostWindowDestroying(void *hwnd)
{
    front::WindowDestroying(reinterpret_cast<front::D3D9Window>(hwnd));
}

// SDL loads the Vulkan loader with the first Vulkan window and unloads it with
// the last one, which the engine destroys before it releases the device: the
// device's teardown would then call into an unloaded library. Holding one more
// reference for the rest of the process keeps the loader where it is.
void HoldVulkanLoader()
{
    static bool held = false;
    if (held)
        return;
    if (SDL_Vulkan_LoadLibrary(nullptr))
        held = true;
    else
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
}

// The Legacy adapter's shader cache file: <fs_homepath>/vulkan/shaders.bin.
// Empty (no file) when r_vkShaderCache is off or there is no home path.
std::string ShaderCachePath()
{
    if (!r_vkShaderCache->current.enabled)
        return {};
    const char *home = Dvar_GetString("fs_homepath");
    if (!home || !*home)
        return {};
    const std::string directory = std::string(home) + "/vulkan";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST)
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: no shader cache: cannot create %s\n", directory.c_str());
        return {};
    }
    return directory + "/shaders.bin";
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
} // namespace

void Kisak_VulkanRegisterDvars()
{
    r_backend = Dvar_RegisterEnum("r_backend", s_backendNames, BACKEND_D3D9, DVAR_ARCHIVE | DVAR_LATCH,
                                  "Graphics back end: d3d9 (the system's Direct3D 9, DXVK on Linux) or vulkan "
                                  "(KisakCOD's own Vulkan renderer)");
    r_vkValidation = Dvar_RegisterEnum("r_vkValidation", s_validationNames, 0, DVAR_ARCHIVE | DVAR_LATCH,
                                       "Vulkan validation layer: off, auto (when installed) or require");
    r_vkDevice = Dvar_RegisterInt("r_vkDevice", -1, uint32_t(-1), 63, DVAR_ARCHIVE | DVAR_LATCH,
                                  "Vulkan device: -1 picks one, N the Nth device the Vulkan driver lists");
    r_vkShaderCache = Dvar_RegisterBool("r_vkShaderCache", true, DVAR_ARCHIVE | DVAR_LATCH,
                                        "Keep translated shaders in fs_homepath/vulkan/shaders.bin between runs");
}

int Kisak_VulkanBackendSelected()
{
    Kisak_VulkanRegisterDvars();
    return r_backend->current.integer == BACKEND_VULKAN;
}

IDirect3D9 *Kisak_VulkanDirect3DCreate9()
{
    Kisak_VulkanRegisterDvars();
    // DXVK starts SDL video inside Direct3DCreate9; so does this. The renderer
    // lists display modes before it makes a window. SDL counts the reference
    // and the Win32 layer takes its own.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: SDL video did not start: %s\n", SDL_GetError());
        return nullptr;
    }
    HoldVulkanLoader();
    kisak::win32_window::SetRendererWindowDestroying(HostWindowDestroying);
    front::Host host;
    host.user = &s_host;
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
    s_host.shaderCachePath = ShaderCachePath();
    host.shaderCachePath = s_host.shaderCachePath.empty() ? nullptr : s_host.shaderCachePath.c_str();
    Com_Printf(CON_CHANNEL_GFX, "Graphics back end: Vulkan (validation %s, device %d, shader cache %s)\n",
               s_validationNames[r_vkValidation->current.integer], r_vkDevice->current.integer,
               s_host.shaderCachePath.empty() ? "off" : s_host.shaderCachePath.c_str());
    return front::CreateDirect3D9(host);
}

bool Kisak_VulkanLastTeardown(KisakVulkanTeardown &out)
{
    std::lock_guard<std::mutex> lock(s_host.teardownLock);
    out = s_host.teardown;
    return s_host.teardownSeen;
}
