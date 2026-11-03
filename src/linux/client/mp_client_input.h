#pragma once

// The multiplayer client's window and input hooks src/linux/main.cpp calls.
// Defined in mp_client_input.cpp, which only KisakCOD-mp builds.

// Before Com_Init: makes the renderer's window the game window once created.
void Sys_InstallClientWindow();
// From Win_GetEvent: queue the game window's pending events. Does nothing
// before the renderer has made the window.
void Sys_PumpClientWindow();
// win_input.cpp's IN_DeactivateMouse, which win_local.h does not declare.
void IN_DeactivateMouse();
