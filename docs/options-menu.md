# Settings the port adds to the options menus

The options menus are game assets: multiplayer reads them from `ui_mp.ff`
(main menu) and `common_mp.ff` (in game), singleplayer from `ui.ff`. The fork
does not ship assets, so it cannot ship edited menus. Instead
`src/ui/ui_port_options.cpp` adds rows to the retail menus at run time, when
`UI_AddMenu` registers a menu. It works on every platform the game builds for.

## Field of View (Options > Game)

A slider for `cg_fov`, from 65 to 120 degrees, with the value printed to the
right of the bar, rounded to a whole degree.

- **Where.** Multiplayer: under "Enable console" (the subtitles and crosshair
  rows of the same menu are singleplayer settings that the multiplayer game
  hides). Singleplayer: under "Enable crosshair". The rows below move down one
  row; the tabs on the left and the buttons at the bottom stay put.
- **How it looks.** The row is a copy of the row above it, so it has that menu's
  sizes, colours, highlight and sounds. The copy gets its own highlight
  number, a slider in place of the setting, the caption "Field of View" (plain
  English: the game's string tables have no entry for it), and a text item
  that shows `toint(dvarfloat("cg_fov") + 0.5)`.
- **When it is left out.** When the menu has no row for the anchor setting
  (`monkeytoy` in multiplayer, `cg_drawCrosshair` in singleplayer), the row has
  no caption, or the menu already has an item for `cg_fov` (a mod's own options
  menu). Registering a menu again does not add a second row.
- **Range.** In retail 1.7 and CoD4X, multiplayer `cg_fov` runs 65 to 80
  (read from `iw3mp.exe`). This port's multiplayer `cg_fov` came from the
  decompile with 65 to 120, and the slider covers that range on every server.
  That was the project's choice (2026-11-11): a player on a stock server can
  see wider than retail players there. `cg_fovScale` stays a cheat setting, as
  in retail. Singleplayer registers `cg_fov` from 1 to 160; the slider uses 65
  to 120 there too.
- **Width.** The game keeps the vertical field of view of a 4:3 screen, so a
  wider screen sees more at the sides (`CG_UpdateFov`). `cg_fov` is the
  horizontal angle a 4:3 screen would see.

The new row adds no dvar, so `server_dvar_guard.h` needs no entry for it.

## Checking it without a game window

`KisakCOD-ui-asset-export` (see `docs/history/overnight-review-2026-10-31/ASSETS.md`)
loads `ui_mp.ff` with the real database and writes `menu-inventory.json`. With
a temporary call to `UI_AddPortOptionRows` on each menu before the export, the
`options_game` inventory shows the seven new items (two background pieces, two
highlight pieces with a new highlight number, the caption, the slider and the
value) under "Enable console", and the rows below one row lower. The tool
reads multiplayer zones only, so the singleplayer menu has not been checked
this way.
