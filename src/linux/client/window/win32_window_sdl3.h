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

// The part of a Win32 window procedure the Linux client needs: WM_CREATE and
// WM_DESTROY. Windows sends both from inside CreateWindowExA and DestroyWindow;
// so does this layer. `created` runs once the window exists and IsWindow knows
// it, before CreateWindowExA returns. `destroying` runs before the window goes
// away, while IsWindow still knows it, so whatever borrowed the window can let
// go first. A failed CreateWindowExA and a DestroyWindow of an unknown handle
// call neither. One observer per process; nullptr removes it. The observer is
// called on the thread that creates or destroys the window, without locks held.
struct WindowObserver {
    void (*created)(void *hwnd) = nullptr;
    void (*destroying)(void *hwnd) = nullptr;
};
void SetWindowObserver(const WindowObserver *observer);

// A second WM_DESTROY listener, for the renderer's Vulkan back end: it must
// destroy its Vulkan surface before the window goes, whoever holds the observer
// above. Runs after the observer's `destroying`, under the same rules.
// nullptr removes it.
void SetRendererWindowDestroying(void (*destroying)(void *hwnd));

} // namespace kisak::win32_window
