#pragma once

// r_backend vulkan on Linux and macOS: the renderer's Direct3D 9 calls drawn
// through the Vulkan back end (the D3D9 front, src/gfx_vk/d3d9, on the Legacy
// adapter) instead of DXVK.
//
// Built into a program only with KISAK_BUILD_VULKAN_BACKEND
// (scripts/vulkan_probe/vulkan_backend.cmake), which also defines
// KISAK_VULKAN_BACKEND for the unit that holds R_HW_Direct3DCreate9
// (src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp). That function asks here first; every
// later D3D9 call lands on whichever IDirect3D9 it returned, so nothing else in
// the renderer changes.
//
// The front's host is SDL3, as for DXVK Native's SDL3 WSI: an HWND is the
// SDL_Window * the Win32 window layer made (src/linux/client/window/
// win32_window_sdl3.cpp), an HMONITOR is an SDL_DisplayID, and display modes
// are SDL's. See docs/vulkan-d3d9-front.md.

#include <cstdint>

struct IDirect3D9;

// Registers the back end's dvars (harmless to call again). Latched and archived:
//   r_backend       d3d9 (the system's D3D9: DXVK on Linux) or vulkan
//   r_vkValidation  off, auto (when installed) or require (Vulkan validation layer)
//   r_vkDevice      -1 picks the GPU; N picks the Nth Vulkan device
//   r_vkShaderCache keep translated shaders and pipelines between runs
// For measuring, not saved, and in effect from the next frame (the front's
// Tuning, read at every Present), so one launch can compare settings:
//   r_vkGpuFeed          default (by the kind of GPU), off, on or always
//   r_vkGpuFeedDraws     draws to a part of a frame; 0 is the default, 128
//   r_vkGpuFeedIdleDraws draws to a part while the GPU has nothing left; 0
//                        for no such parts
//   r_vkKeepDrawState    a draw takes up what the one before left (default);
//                        0 makes every draw set everything again
void Kisak_VulkanRegisterDvars();

// Registers the dvars; whether r_backend is vulkan.
int Kisak_VulkanBackendSelected();

// The front's IDirect3D9, after starting SDL video (R_EnumDisplayModes lists
// modes before any window exists). Null when SDL video cannot start, with the
// reason printed; R_PreCreateWindow then reports that Direct3D failed to
// initialize. There is no fallback to DXVK: the setting asked for Vulkan.
IDirect3D9 *Kisak_VulkanDirect3DCreate9();

// Logs the Vulkan device's frame statistics (the `frame stats:` lines of
// docs/vulkan-d3d9-front.md) since it was made or since they were last
// started again; with reset they start again here. Returns how many devices
// there were: 0 with r_backend d3d9. The renderer's r_vkStats command
// (src/gfx_d3d/r_cmds.cpp) calls it. Any thread.
unsigned int Kisak_VulkanLogStats(bool reset);

// The last Vulkan device's final statistics, as its core reported them at
// teardown (validation messages about the teardown included). False until a
// device has been destroyed.
struct KisakVulkanTeardown
{
    uint64_t framesPresented;
    uint64_t framesSkipped;
    uint32_t validationErrors;
    uint32_t validationWarnings;
};
bool Kisak_VulkanLastTeardown(KisakVulkanTeardown &out);
