#pragma once

// gfx_vk: the shared Vulkan core of the planned Vulkan renderer back end.
//
// The renderer plan (section 3) puts one engine-independent Vulkan core under
// both the Legacy adapter (the D3D9-shaped seam in src/gfx_d3d/rhi/r_rhi.h) and
// the Modern path. This is its first piece: instance, device, queue, swapchain,
// per-frame command buffers and synchronisation, a cleared and presented frame,
// resize, GPU timestamps around each pass, and a read-back of a presented frame
// for checks. It knows nothing about the engine, materials or D3D9; the window
// system is handed in (WindowSystem below), so the same code serves SDL3 on
// Linux and Windows.
//
// Requirements on the driver: Vulkan 1.3 (dynamic rendering and
// synchronization2 are core there), VK_KHR_swapchain, and one queue family that
// does graphics and presents to the window. Mesa's lavapipe, Intel ANV and
// NVIDIA's driver for the RTX 2080 Super all meet them.
//
// Threading: one thread owns a Core and calls everything on it.
// See docs/vulkan-core.md.

#include "vk_dispatch.h"

#include <cstdint>

namespace gfx_vk
{
// What the core needs from the window system. Every member is required.
struct WindowSystem
{
    // The Vulkan loader's entry point (SDL_Vulkan_GetVkGetInstanceProcAddr).
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
    // Instance extensions the window system needs (SDL_Vulkan_GetInstanceExtensions).
    const char *const *instanceExtensions;
    uint32_t instanceExtensionCount;
    void *user;
    // Creates the window's surface on the instance (SDL_Vulkan_CreateSurface).
    bool (*createSurface)(void *user, VkInstance instance, VkSurfaceKHR *surface);
    // The window's size in pixels; 0 x 0 while it is minimised.
    void (*getPixelSize)(void *user, uint32_t *width, uint32_t *height);
};

enum class Validation
{
    Off,      // never load the validation layer
    IfFound,  // load VK_LAYER_KHRONOS_validation when it is installed
    Required, // fail CreateCore when it is not installed
};

struct CoreDesc
{
    const char *applicationName = "KisakCOD";
    Validation validation = Validation::Off;
    // Index into the physical device list as LogDevices prints it; -1 picks the
    // best device that meets the requirements (discrete, then integrated GPU,
    // then virtual, then CPU).
    int deviceIndex = -1;
    bool vsync = true;
    // Ask for swapchain images that can be copied from, so CaptureNextFrame works.
    bool allowCapture = false;
    // Where the core's messages go (one line per call, no newline). Null: nowhere.
    void (*log)(void *user, const char *line) = nullptr;
    void *logUser = nullptr;
};

struct DeviceInfo
{
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorId;
    uint32_t deviceId;
    VkPhysicalDeviceType type;
    char driverName[VK_MAX_DRIVER_NAME_SIZE];
    char driverInfo[VK_MAX_DRIVER_INFO_SIZE];
};

struct SwapchainInfo
{
    uint32_t width;
    uint32_t height;
    VkFormat format;
    VkColorSpaceKHR colorSpace;
    VkPresentModeKHR presentMode;
    uint32_t imageCount;
    bool canCapture;
};

// GPU time of the passes recorded so far, from timestamp queries. Empty
// (supported false) when the queue has no timestamp bits.
struct PassTimings
{
    bool supported;
    uint64_t samples;
    double lastMs;
    double minMs;
    double maxMs;
    double totalMs;
};

struct CoreStats
{
    uint64_t framesPresented;
    uint64_t framesSkipped;   // window minimised or swapchain out of date
    uint32_t swapchainBuilds; // the first one included
    uint32_t validationErrors;
    uint32_t validationWarnings;
    PassTimings clearPass;
};

// A presented frame copied back to memory: tightly packed rows of 4 bytes per
// pixel in the swapchain's format (B8G8R8A8 or R8G8B8A8).
struct Capture
{
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    VkFormat format;
};

class Core;

// Creates the instance, picks and creates the device, and builds the first
// swapchain. On failure returns null and writes the reason into error.
Core *CreateCore(const CoreDesc &desc, const WindowSystem &window, char *error, size_t errorSize);
// Waits for the GPU and destroys everything. finalStats, if given, receives the
// statistics after teardown, so validation messages about destruction count.
void DestroyCore(Core *core, CoreStats *finalStats = nullptr);

// Lists the physical devices with the index CoreDesc::deviceIndex takes and why
// each one does or does not meet the requirements. Needs only an instance, so it
// works even where CreateCore fails.
bool LogDevices(const CoreDesc &desc, const WindowSystem &window, char *error, size_t errorSize);

const char *LastError(const Core *core);
uint32_t InstanceApiVersion(const Core *core);
bool ValidationEnabled(const Core *core);
// Validation includes synchronisation checks (VK_EXT_validation_features).
bool SynchronizationValidationEnabled(const Core *core);
const DeviceInfo &Device(const Core *core);
const SwapchainInfo &Swapchain(const Core *core);
const CoreStats &Stats(const Core *core);

// The window may have changed size (or was minimised or restored): the next
// frame checks the surface and rebuilds the swapchain if the size differs.
void NotifyResize(Core *core);

// One frame, as the first pass of the future pass list: acquire, clear the
// whole swapchain image to rgba (0..1, written as-is: the swapchain is UNORM,
// like the gamma-space retail renderer), present.
//   Ok       the frame was presented.
//   Skipped  nothing was drawn (minimised, or the swapchain was rebuilt).
//   Failed   LastError says why; the core cannot continue.
enum class FrameResult
{
    Ok,
    Skipped,
    Failed,
};
FrameResult ClearAndPresent(Core *core, const float rgba[4]);

// Copies the next presented frame back to memory. The frame after the call to
// ClearAndPresent waits for the GPU, so use it for checks, not every frame.
// False when the swapchain cannot be copied from (CoreDesc::allowCapture off, or
// the surface does not allow it).
bool CaptureNextFrame(Core *core);
// The last capture, or pixels null if none has completed.
Capture LastCapture(const Core *core);

// Waits until the GPU is idle (before reading stats at the end of a run).
void WaitIdle(Core *core);

const char *PresentModeName(VkPresentModeKHR mode);
const char *FormatName(VkFormat format);
const char *DeviceTypeName(VkPhysicalDeviceType type);
} // namespace gfx_vk
