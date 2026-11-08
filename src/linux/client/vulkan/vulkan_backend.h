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

// Registers the back end's dvars (latched and archived; harmless to call again):
//   r_backend       d3d9 (the system's D3D9: DXVK on Linux) or vulkan
//   r_vkValidation  off, auto (when installed) or require (Vulkan validation layer)
//   r_vkDevice      -1 picks the GPU; N picks the Nth Vulkan device
void Kisak_VulkanRegisterDvars();

// Registers the dvars; whether r_backend is vulkan.
int Kisak_VulkanBackendSelected();

// The front's IDirect3D9, after starting SDL video (R_EnumDisplayModes lists
// modes before any window exists). Null when SDL video cannot start, with the
// reason printed; R_PreCreateWindow then reports that Direct3D failed to
// initialize. There is no fallback to DXVK: the setting asked for Vulkan.
IDirect3D9 *Kisak_VulkanDirect3DCreate9();

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
