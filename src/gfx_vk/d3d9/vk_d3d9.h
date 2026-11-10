#pragma once

// The D3D9 front of the Vulkan Legacy adapter: IDirect3D9, IDirect3DDevice9 and
// the resource interfaces, implemented on gfx_vk::legacy (vk_legacy.h).
//
// The renderer keeps calling Direct3D 9 exactly as it does with d3d9.dll or
// DXVK: through the R_HW_* functions in src/gfx_d3d/rhi and the D3D9
// interfaces. With r_backend vulkan, R_HW_Direct3DCreate9 returns the object
// this file creates instead of the system's, and every later call lands here.
// Methods the renderer never calls answer D3DERR_INVALIDCALL (or a harmless
// default) and are listed in docs/vulkan-d3d9-front.md.
//
// The D3D9 declarations come from the same place the renderer's do: the
// Windows SDK on Windows, and the native D3D9 boundary (DXVK Native's headers
// through <linux/d3d9_types.h>, KISAK_LINUX_NATIVE_D3D9) on Linux and macOS, so
// the vtables, types and calling conventions are the renderer's own.

#if defined(_WIN32)
#include <d3d9.h>
#else
#include <linux/d3d9_types.h>
#endif

#if !defined(KISAK_GFX_GXM)
#include <gfx_vk/vk_core.h>
#endif

#include <cstdint>

namespace gfx_vk::d3d9
{
// The window and point types the D3D9 interfaces are declared with. On Linux
// and macOS the boundary reads DXVK's headers with these renamed (DXVK's HWND
// is void *, the engine's is not); the front's overrides must use DXVK's.
#if defined(_WIN32)
using D3D9Window = HWND;
using D3D9Point = POINT;
#else
using D3D9Window = KisakDxvkHWND;
using D3D9Point = KisakDxvkPOINT;
#endif

// Settings the program may change while a device exists, so that one run can
// compare them (the engine's r_vkGpuFeed dvars and r_vkKeepDrawState; perf_ab
// flips those). The front asks for them at every Present. The PS Vita's GXM
// back end has none.
struct Tuning
{
    // The GPU feed (lg::GpuFeed): -1 the device's own choice (its kind of
    // GPU, KISAK_VK_GPU_FEED), 0 off, 1 on, 2 a part after every draw.
    int gpuFeed = -1;
    // Draws to a part; 0 the device's own (KISAK_VK_GPU_FEED_DRAWS, else 128).
    uint32_t gpuFeedDraws = 0;
    // The idle rule's first part (lg::GpuFeed::idleDraws); 0 for no such rule.
    uint32_t gpuFeedIdleDraws = 0;
    // A draw takes up the pipeline, constants and binds the draw before it
    // left (lg::SetKeepDrawState). False makes every draw set them all again.
    bool keepDrawState = true;
};

// What the front needs from the program around it: the window system, display
// modes and the monitor. D3D9 answers these from the OS; the front asks.
struct Host
{
    void *user = nullptr;
#if !defined(KISAK_GFX_GXM)
    // The window system for the window CreateDevice is given (the device
    // window, or the focus window when that is null). On Linux and macOS the
    // HWND is the SDL_Window the window host created.
    bool (*windowSystem)(void *user, D3D9Window window, WindowSystem &out) = nullptr;
#endif
    // Display modes of the one adapter, for D3DFMT_X8R8G8B8 (the only format
    // the renderer enumerates). Null: one mode, the current one.
    uint32_t (*modeCount)(void *user) = nullptr;
    bool (*mode)(void *user, uint32_t index, uint32_t &width, uint32_t &height, uint32_t &refreshRate) = nullptr;
    bool (*currentMode)(void *user, uint32_t &width, uint32_t &height, uint32_t &refreshRate) = nullptr;
    HMONITOR (*monitor)(void *user) = nullptr;
    // Where the window's client area starts on its monitor, in pixels, for
    // GetFrontBufferData into a display-sized surface (windowed screenshots).
    // Null: the monitor's top-left corner.
    bool (*windowOrigin)(void *user, D3D9Window window, int32_t &x, int32_t &y) = nullptr;
    // One line per call, no newline. Null: nowhere.
    void (*log)(void *user, const char *line) = nullptr;

    // The Legacy adapter's shader cache file (lg::DeviceDesc::shaderCachePath):
    // translated shaders kept between runs. Null: none (translated shaders are
    // still shared within the process).
    const char *shaderCachePath = nullptr;

#if !defined(KISAK_GFX_GXM)
    // How the Vulkan core is created at CreateDevice (vsync follows the
    // presentation interval). The PS Vita's GXM back end (KISAK_GFX_GXM) has
    // no core and none of these.
    Validation validation = Validation::Off;
    int deviceIndex = -1;
    const char *pipelineCachePath = nullptr;
    // Called when a device's core is destroyed, with its final statistics
    // (validation messages about the teardown included). Null: not called.
    void (*coreDestroyed)(void *user, const CoreStats &stats) = nullptr;
    // Fills in the settings above, which arrive at their defaults. Called
    // under the front's lock on the thread that presents. Null: defaults.
    void (*tuning)(void *user, Tuning &out) = nullptr;
#endif
};

// The IDirect3D9 the renderer asks for adapters, caps and formats, and for its
// device. The host must outlive it and every object it creates. Never null.
IDirect3D9 *CreateDirect3D9(const Host &host);

// Statistics of a device the front created (null for any other device).
struct DeviceCounters
{
    uint64_t presents;
    uint64_t presentsSkipped;
    uint64_t unsupportedCalls; // calls answered D3DERR_INVALIDCALL as unimplemented
    uint32_t liveObjects;      // resources, shaders, declarations and queries not yet destroyed
    uint32_t validationErrors; // the core's so far
    bool validation;           // the validation layer is loaded
    uint64_t submissions;      // queue submissions so far (Vulkan back end)
    uint64_t fenceFlushes;     // of those, frames submitted ahead of their Present by a swap fence poll
    uint64_t feedFlushes;      // of those, parts of frames submitted while the frame was being drawn
    uint64_t feedIdleFlushes;  // of those, parts sent early to a GPU with nothing left (Tuning::gpuFeedIdleDraws)
    // Occlusion queries count samples. False where the back end cannot, and
    // with KISAK_VK_OCCLUSION_QUERIES=0: a finished query then reports every
    // sample as passed (0xffffff on Vulkan).
    bool occlusionCounted;
    // Vulkan queries begun for them (a query takes one for every run of its
    // draws that no pass end interrupts), and the pools of 256 made for them.
    uint64_t occlusionCounts;
    uint32_t occlusionPools;
    // What they answered where they count: results worked out for issued
    // queries, those of them with no sample, and all their samples.
    uint64_t occlusionResults;
    uint64_t occlusionResultsZero;
    uint64_t occlusionSamples;
};
bool Counters(IDirect3DDevice9 *device, DeviceCounters &out);

// Logs every live device's frame statistics through its host (the lines a
// device logs when it is destroyed: `frame stats:`, `slow frame`, `record
// thread:`, `occlusion queries:`), counted since the device was created or
// since the last call that reset them. With reset, the next count starts here, so a
// program can measure one stretch of a run (the engine's r_vkStats command).
// Any thread. Returns how many devices there were.
uint32_t LogDeviceStats(bool reset);

// The window is about to be destroyed (WM_DESTROY on Windows, before
// SDL_DestroyWindow elsewhere). Every device the front made for it lets go of
// its Vulkan swapchain and surface now, while the window still exists; a
// surface must not outlive its window. The devices stay usable until they are
// released, but present nothing. Returns how many devices used the window.
uint32_t WindowDestroying(D3D9Window window);
} // namespace gfx_vk::d3d9
