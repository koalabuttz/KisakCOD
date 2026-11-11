#pragma once

// The GPU under the D3D9 front's devices.
//
// The front draws through the Legacy adapter's API (gfx_vk::legacy, vk_legacy.h)
// and nothing else, except when a device is created and destroyed: something
// has to bring up the GPU for the window and hand the front a Legacy device on
// it. This seam is that something, so the same front runs on more than one
// implementation of the API:
//
//   vk_d3d9_backend_vulkan.cpp   a Vulkan core per device (gfx_vk, vk_core.h),
//                                on Windows, Linux and macOS
//   src/gfx_gxm (KISAK_GFX_GXM)  the PS Vita's sceGxm and its one screen
//
// A build links exactly one. Everything here is called under FrontLock,
// except BackendPollProgress.

#include "vk_d3d9.h"

#include <gfx_vk/legacy/vk_legacy.h>

#include <cstdint>
#include <string>

namespace gfx_vk::d3d9
{
namespace lg = gfx_vk::legacy;

struct Backend;

struct BackendDesc
{
    const Host *host = nullptr;
    D3D9Window window{};
    bool vsync = true;
    // The back buffer the program asked for; 0 takes the window's size.
    uint32_t width = 0;
    uint32_t height = 0;
    // Its samples a pixel (D3DPRESENT_PARAMETERS::MultiSampleType); 1 on a
    // back end that has none.
    uint32_t samples = 1;
};

// Brings up the GPU for the window and a Legacy device on it. Null with the
// reason (one line, no prefix).
Backend *OpenBackend(const BackendDesc &desc, std::string &error);
// Logs the back end's statistics, destroys the Legacy device and then the GPU
// side. Null is ignored.
void CloseBackend(Backend *backend);
// Logs the back end's frame statistics since it was opened or since they
// were last reset (vk_d3d9.h LogDeviceStats); with reset, starts them again.
void BackendLogStats(Backend *backend, bool reset);
// Called at every Present, before the frame is presented: the back end takes
// up the host's settings (Host::tuning) if they changed.
void BackendFrame(Backend *backend);

lg::Device *LegacyDevice(Backend *backend);
// "<GPU name>" for the device line the front logs.
const char *BackendDeviceName(const Backend *backend);
// The window is about to be destroyed (vk_d3d9.h WindowDestroying).
void DetachBackendWindow(Backend *backend);
// What GetAvailableTextureMem answers.
uint64_t BackendTextureMemory(const Backend *backend);
// The window's size in pixels, for a Reset that asks for 0 x 0. False when
// the back end cannot tell.
bool BackendWindowSize(const Backend *backend, uint32_t &width, uint32_t &height);
// Fills the back end's part of DeviceCounters (validation, submissions).
void BackendCounters(const Backend *backend, DeviceCounters &out);
// Called without FrontLock, from any thread, before a query is asked for its
// result under the lock: a back end whose answer takes a system call makes
// it here, so that a thread polling a fence does not hold the lock every
// other thread's D3D9 calls need while it is in the kernel. The back end
// outlives the call (the query keeps its device alive). Null is ignored.
void BackendPollProgress(Backend *backend);
// The Modern path's passes over the scene drawn into these surfaces
// (vk_d3d9.h ModernSceneDone). Refused when the back end has none or they
// cannot run; the back end logs why, once per reason.
ModernResult BackendModernScene(Backend *backend, lg::Surface *color, lg::Surface *depth,
                                const ModernSceneView &view);
} // namespace gfx_vk::d3d9
