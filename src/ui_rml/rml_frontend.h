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
// These hooks are called from the stock UI only when KISAK_RMLUI is defined.

struct UiContext;

// Registers ui_rml and the front-end's console commands (UI_RegisterDvars).
void RmlFrontend_RegisterDvars();

// True when ui_rml asks for the classic mode. UI_Refresh then calls
// RmlFrontend_PaintClassic instead of Menu_PaintAll.
bool RmlFrontend_WantsClassicMenus();

// Paints the open menus through RmlUi, in Menu_PaintAll's order. Falls back to
// Menu_PaintAll if RmlUi could not start.
void RmlFrontend_PaintClassic(UiContext *dc);

// Releases every RmlUi document and texture (UI_Shutdown: the menus and the
// materials they point at are about to go). RmlUi itself stays up.
void RmlFrontend_Shutdown();
