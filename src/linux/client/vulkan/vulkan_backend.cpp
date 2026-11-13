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
#include <gfx_vk/modern/vk_modern.h>
#include <gfx_d3d/rhi/r_rhi_modern.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/window/win32_window_sdl3.h>

#if KISAK_LINUX_SDL_MAJOR != 3
#error "the Vulkan back end's host is SDL3 (KISAK_LINUX_SDL_MAJOR=3)"
#endif
#include <SDL3/SDL_vulkan.h>

#include "vulkan_backend.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
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
    // Set fullscreen by HostFullscreen, until it is set back or destroyed
    // (WM_DESTROY may come from another thread than the front's calls).
    std::atomic<SDL_Window *> fullscreenWindow{nullptr};
    std::vector<DisplayMode> modes;
    std::string shaderCachePath;   // what Host::shaderCachePath points at
    std::string pipelineCachePath; // what Host::pipelineCachePath points at
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

// ---- Fullscreen ------------------------------------------------------------------

// The window fullscreen on the primary display at the closest mode SDL has, as
// D3D9 (and DXVK's SDL window system) do for a fullscreen device. SDL gives the
// desktop its mode back when the window goes back to windowed, when it is
// destroyed and, by default, when it loses focus (it minimizes then).
bool HostFullscreen(void *, front::D3D9Window handle, const front::FullscreenMode *mode)
{
    if (!mode)
    {
        SDL_Window *const window = s_host.fullscreenWindow.exchange(nullptr);
        if (window && !SDL_SetWindowFullscreen(window, false))
        {
            Com_Printf(CON_CHANNEL_GFX, "Vulkan: SDL_SetWindowFullscreen(off) failed: %s\n", SDL_GetError());
            return false;
        }
        return true;
    }
    SDL_Window *const window = reinterpret_cast<SDL_Window *>(handle);
    const SDL_DisplayID display = Display();
    if (!window || !SDL_GetWindowID(window) || !display)
        return false;
    const SDL_DisplayMode *const desktop = SDL_GetDesktopDisplayMode(display);
    const int width = mode->width ? int(mode->width) : (desktop ? desktop->w : 0);
    const int height = mode->height ? int(mode->height) : (desktop ? desktop->h : 0);
    SDL_DisplayMode closest{};
    // A refresh rate of 0 takes the desktop's.
    const bool found = width > 0 && height > 0
        && SDL_GetClosestFullscreenDisplayMode(display, width, height, float(mode->refreshRate), false, &closest);
    if (!found)
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: no %dx%d display mode at %u Hz; fullscreen at the desktop's mode\n",
                   width, height, mode->refreshRate);
    s_host.fullscreenWindow.store(window);
    // Null: fullscreen at the desktop's mode (borderless, the window covering it).
    bool ok = SDL_SetWindowFullscreenMode(window, found ? &closest : nullptr);
    ok = SDL_SetWindowFullscreen(window, true) && ok;
    if (!ok)
        Com_Printf(CON_CHANNEL_GFX, "Vulkan: fullscreen failed: %s\n", SDL_GetError());
    (void)SDL_SyncWindow(window);
    return ok && found;
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
    SDL_Window *destroyed = static_cast<SDL_Window *>(hwnd);
    s_host.fullscreenWindow.compare_exchange_strong(destroyed, nullptr);
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

// Where the caches live: <fs_homepath>/vulkan, holding the Legacy adapter's
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
    const std::string directory = std::string(home) + "/vulkan";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST)
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
    out.gpuFeedDraws = uint32_t(std::max(0, r_vkGpuFeedDraws->current.integer));
    out.gpuFeedIdleDraws = uint32_t(std::max(0, r_vkGpuFeedIdleDraws->current.integer));
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
    view.x = uint32_t(std::max(0, scene->x));
    view.y = uint32_t(std::max(0, scene->y));
    view.width = uint32_t(std::max(0, scene->width));
    view.height = uint32_t(std::max(0, scene->height));
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
                                        "Keep translated shaders and Vulkan pipelines in fs_homepath/vulkan "
                                        "(shaders.bin, pipelines.bin) between runs");
    // Measuring switches: not saved, and in effect from the next frame, so
    // one launch can compare settings (perf_ab, docs/perf-ab.md).
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

int Kisak_VulkanBackendSelected()
{
    Kisak_VulkanRegisterDvars();
    if (r_backend->current.integer == BACKEND_VULKAN)
        return 1;
    // The Modern path's hooks belong to the Vulkan back end.
    R_SetModernHooks(nullptr);
    return 0;
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
    host.fullscreen = HostFullscreen;
    host.log = HostLog;
    host.validation = ValidationSetting();
    host.deviceIndex = r_vkDevice->current.integer;
    host.coreDestroyed = HostCoreDestroyed;
    host.tuning = HostTuning;
    const std::string cacheDirectory = CacheDirectory();
    s_host.shaderCachePath = cacheDirectory.empty() ? std::string() : cacheDirectory + "/shaders.bin";
    s_host.pipelineCachePath = cacheDirectory.empty() ? std::string() : cacheDirectory + "/pipelines.bin";
    host.shaderCachePath = s_host.shaderCachePath.empty() ? nullptr : s_host.shaderCachePath.c_str();
    host.pipelineCachePath = s_host.pipelineCachePath.empty() ? nullptr : s_host.pipelineCachePath.c_str();
    Com_Printf(CON_CHANNEL_GFX, "Graphics back end: Vulkan (validation %s, device %d, shader cache %s, visuals %s)\n",
               s_validationNames[r_vkValidation->current.integer], r_vkDevice->current.integer,
               s_host.shaderCachePath.empty() ? "off" : s_host.shaderCachePath.c_str(),
               s_visualsNames[r_visuals->current.integer]);
    s_modernRefused.store(false, std::memory_order_relaxed);
    R_SetModernHooks(r_visuals->current.integer == VISUALS_MODERN ? &s_modernHooks : nullptr);
    return front::CreateDirect3D9(host);
}

unsigned int Kisak_VulkanLogStats(bool reset)
{
    return front::LogDeviceStats(reset);
}

bool Kisak_VulkanLastTeardown(KisakVulkanTeardown &out)
{
    std::lock_guard<std::mutex> lock(s_host.teardownLock);
    out = s_host.teardown;
    return s_host.teardownSeen;
}
