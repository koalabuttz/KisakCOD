#pragma once

// A game controller in the multiplayer client (KISAK_LINUX_GAMEPAD): the
// engine's side of src/linux/client/input. mp_client_input.cpp calls these
// from the engine's input entry points; without the build option none of this
// is compiled and the client has no controller code.

namespace kisak::mp_client {

// IN_Init: the in_gamepad dvars and the gamepad_binddefaults command.
void GamepadInit();
// IN_Shutdown: lets go of every key and closes the controller.
void GamepadShutdown();
// Sys_PumpClientWindow, on the window's thread: one reading of the
// controller, as key events in the engine's queue.
void GamepadPump();
// The game window's hand-over to CL_MouseEvent, with the controller's look
// motion and menu cursor merged in. Same contract as CL_MouseEvent.
int GamepadMouse(int x, int y, int dx, int dy);
// IN_Frame, after the window's own frame. `active` is "the game window has
// the focus"; without it everything held is let go.
void GamepadFrame(bool active);
// IN_SetCursorPos: the engine moved the cursor.
void GamepadSetCursor(int x, int y);

} // namespace kisak::mp_client
