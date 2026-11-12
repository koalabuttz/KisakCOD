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
// Today there is one row: a Field of View slider for cg_fov in Options > Game.

struct menuDef_t;

// Adds the port's rows to `menu` when it is one of the menus that gets them.
// Calling it again for a menu that already has them does nothing.
void UI_AddPortOptionRows(menuDef_t *menu);
