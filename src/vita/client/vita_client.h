#pragma once

// Pieces shared by the PS Vita multiplayer client's own units (src/vita/client).
// See docs/vita-client.md.

#if !defined(__vita__) || !defined(KISAK_VITA_MP_CLIENT)
#error "vita/client is the PS Vita multiplayer client only"
#endif

// The Vita's screen: the one window the renderer gets.
inline constexpr int kVitaScreenWidth = 960;
inline constexpr int kVitaScreenHeight = 544;

// One line (no newline) to the engine console, which reaches console.log.
void Vita_ClientLog(const char *line);

// WM_CREATE and WM_DESTROY of the game window (null: destroyed). Sets the
// engine's window handle, as win_wndproc.cpp does on Windows.
void Vita_ClientWindowChanged(void *hwnd);

// Whether hwnd is the open game window (vita_win32_window.cpp).
bool Vita_IsScreenWindow(const void *hwnd);

// WM_DESTROY for the renderer: the D3D9 front's devices let go of the screen
// before the window goes (vita_d3d9.cpp).
void Vita_RendererWindowDestroying(void *hwnd);
