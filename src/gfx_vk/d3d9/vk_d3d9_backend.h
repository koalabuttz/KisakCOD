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
// A build links exactly one. Everything here is called under FrontLock.

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
};

// Brings up the GPU for the window and a Legacy device on it. Null with the
// reason (one line, no prefix).
Backend *OpenBackend(const BackendDesc &desc, std::string &error);
// Logs the back end's statistics, destroys the Legacy device and then the GPU
// side. Null is ignored.
void CloseBackend(Backend *backend);

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
} // namespace gfx_vk::d3d9
