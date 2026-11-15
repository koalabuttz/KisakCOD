#pragma once

// The RmlUi front-end (opt-in, KISAK_RMLUI; docs/rmlui-frontend.md).
//
// RmlUi (https://github.com/mikke89/RmlUi) lays out and draws documents
// written in RML and RCSS, HTML and CSS look-alikes. This front-end gives it
// the engine's own drawing (R_AddCmdDrawUiTriangles, so every back end draws
// it), the game's fonts and materials, and the engine's file system.
//
// The first use is the classic mode (ui_rml 1): the stock menus, as the
// fastfiles define them, rebuilt every frame as an RmlUi document and drawn by
// RmlUi instead of the stock painter. The stock menu code still owns input,
// focus, scripts and sounds, so the menus behave exactly as before; only the
// drawing moves. It is a proof that RmlUi draws the original menus with the
// game's own art on every renderer, before new screens are written in RML.
//
// The second use is screens: new menus written in RML (rml_screens.h), such
// as the graphics settings (ui_rmlOpen graphics). A screen opens over the
// stock menus and takes the keys and the mouse while it is open.
//
// These hooks are called from the stock UI only when KISAK_RMLUI is defined.

struct UiContext;

// Registers ui_rml and the front-end's console commands (UI_RegisterDvars):
// ui_rmlStats, ui_rmlOpen <screen>, ui_rmlClose.
void RmlFrontend_RegisterDvars();

// Paints the open menus (UI_Refresh, in place of Menu_PaintAll): through
// RmlUi when ui_rml asks for the classic mode, else the stock way; then the
// open screen, if any. Falls back to Menu_PaintAll if RmlUi could not start.
void RmlFrontend_PaintMenus(UiContext *dc);

// Input for the open screen (UI_KeyEvent, UI_MouseEvent). True when a screen
// took it, and the stock menus must not see it. The mouse is in the full
// screen's pixels, as UI_MouseEvent gets it.
bool RmlFrontend_KeyEvent(int key, int down);
bool RmlFrontend_MouseEvent(int x, int y);
bool RmlFrontend_ScreenOpen();

// Releases every RmlUi document and texture (UI_Shutdown: the menus and the
// materials they point at are about to go). RmlUi itself stays up, and a
// screen that was open opens again with the next UI.
void RmlFrontend_Shutdown();
