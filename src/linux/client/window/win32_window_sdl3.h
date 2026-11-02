#pragma once

// Pieces of win32_window_sdl3.cpp that its asset-free test checks directly.
// The Win32 calls themselves are declared in src/linux/win32_window.h.

#include <cstddef>
#include <cstdint>

namespace kisak::win32_window {

// The SDL_WindowFlags CreateWindowExA asks SDL for, given the Win32 styles.
// Every window is a Vulkan window, because DXVK's SDL3 WSI creates its surface
// with SDL_Vulkan_CreateSurface. WS_VISIBLE decides shown or hidden, WS_POPUP a
// borderless window and WS_EX_TOPMOST an always-on-top one. Fullscreen is not a
// window style: D3D9 asks for it through the device (DXVK switches the window).
std::uint64_t WindowFlagsForStyle(std::uint32_t style, std::uint32_t exStyle);

// Windows created by CreateWindowExA and not yet destroyed.
std::size_t OpenWindowCount();

} // namespace kisak::win32_window
