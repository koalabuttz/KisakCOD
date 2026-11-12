#pragma once

// Rows this port adds to the game's own options menus.
//
// The menus come from the game's fast files (ui_mp.ff and common_mp.ff for
// multiplayer, ui.ff for singleplayer), which the fork must not ship. So the
// rows are made at run time, when a menu is registered, as copies of a row the
// same menu already has: they look like the retail rows around them, and a
// menu that does not have the expected row (a mod's own options menu) is left
// as it is.
//
// Options > Game gets Field of View, FPS Counter and Max Frame Rate rows. Two
// pages are added, each with a tab: Display (fullscreen, fullscreen mode,
// renderer) in the Options column and Controller (the Linux controller's
// settings) in the Controls column. See docs/options-menu.md.

struct UiContext;
struct menuDef_t;

// Adds the port's rows to `menu` when it is one of the menus that gets them,
// the port's tabs to its tab column, and registers in `dc` the pages built
// from it. Call it after `menu` itself is registered in `dc`. Calling it again
// for a menu that already has them adds nothing.
void UI_AddPortOptions(UiContext *dc, menuDef_t *menu);

// Puts back what UI_AddPortOptions added to `menu` when the menu's contents
// have been replaced since, and builds a port page again when its retail page
// has other contents now. Menus_OpenByName (the `open` script command) calls
// it before it opens `menu`. The database can replace a registered menu's
// contents: back at the main menu after a map, ui_mp.ff's options menus
// replace common_mp.ff's same-named ones when the zone has finished loading,
// which is after the UI registered them.
void UI_RefreshPortOptions(UiContext *dc, menuDef_t *menu);
