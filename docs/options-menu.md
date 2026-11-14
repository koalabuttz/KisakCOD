# Settings the port adds to the options menus

The options menus are game assets: multiplayer reads them from `ui_mp.ff`
(main menu) and `common_mp.ff` (in game), singleplayer from `ui.ff`. The fork
does not ship assets, so it cannot ship edited menus. Instead
`src/ui/ui_port_options.cpp` adds rows, pages and tabs to the retail menus at
run time, right after `UI_AddMenu` registers a menu. It works on every platform
the game builds for.

Everything it adds is a copy of something the same menus already have (a row,
a tab, a page, the Apply button), so it keeps their sizes, colours, highlight
and sounds. Captions are plain English: the game's string tables have no
entries for them.

## Options > Game: three rows

Under "Enable console" in multiplayer (the subtitles and crosshair rows of the
same menu are singleplayer settings that multiplayer hides), under "Enable
crosshair" in singleplayer. The rows below move down; the tabs on the left and
the buttons at the bottom stay put.

| Row | Setting | Choices |
| --- | --- | --- |
| Field of View | `cg_fov` | slider 65 to 120, value shown rounded to a whole degree |
| FPS Counter | `cg_drawFPS` | Off, On, With ranges, Detailed |
| Max Frame Rate | `com_maxfps` | Unlimited, 30, 60, 85, 100, 120, 125, 144, 165, 200, 240, 250, 333 |

- **FOV range.** In retail 1.7 and CoD4X, multiplayer `cg_fov` runs 65 to 80
  (read from `iw3mp.exe`). This port's multiplayer `cg_fov` came from the
  decompile with 65 to 120, and the slider covers that range on every server.
  That was the project's choice (2026-11-11): a player on a stock server can
  see wider than retail players there. `cg_fovScale` stays a cheat setting, as
  in retail. Singleplayer registers `cg_fov` from 1 to 160; the slider uses 65
  to 120 there too. `cg_fov` is the horizontal angle a 4:3 screen would see;
  wider screens see more at the sides (`CG_UpdateFov`).
- **Frame rates.** 125, 250 and 333 are the values players pick for their
  effect on movement; the rest are common refresh rates. A value typed in the
  console that is not in the list still applies.

## Options > Display: a new page

A tab under "Reset system defaults" in the Options column. It works when
Graphics does: in a game it is greyed out like Graphics unless the server
allows graphics changes, because all three settings need a `vid_restart`.

| Row | Setting | Choices |
| --- | --- | --- |
| Fullscreen | `r_fullscreen` | No, Yes |
| Fullscreen Mode | `r_fullscreenMode` | Exclusive, Borderless window |
| Renderer | `r_backend` | Direct3D 9, Vulkan (only in builds with the Vulkan back end) |

A change lights the page's Apply button (bottom right), a copy of the one in
Options > Graphics: it asks first, then restarts the renderer. As on Graphics,
it stays lit until the page is opened again, even if the setting is changed
back. Opening the page runs
what Options > Graphics runs when it opens (`options_graphics.cfg`), because
that Apply also sets the Graphics page's settings from their menu copies.

**Borderless window** (`r_fullscreenMode borderless`, port dvar, archived,
applied by `vid_restart`): with `r_fullscreen 1`, the game opens a window
without a border that covers the monitor at the desktop's resolution, instead
of changing the display mode. Switching away from the game is instant and
the display never changes mode. `r_mode` and `r_displayRefresh` do not apply
in this mode; the game draws at the desktop resolution. `R_SetWndParms` in
`src/gfx_d3d/r_init.cpp`. A `KISAK_PURE` build has neither the dvar nor the
row.

## Controls > Controller: a new page

A tab under "Set default controls" in the Controls column, in builds with the
Linux controller support (`KISAK_LINUX_GAMEPAD`); elsewhere neither the page
nor the tab exists.

| Row | Setting | Range |
| --- | --- | --- |
| Use Controller | `in_gamepad` | No, Yes |
| Look Speed | `in_gamepadLookSpeed` | 200 to 5000 |
| Vertical Speed % | `in_gamepadLookPitchRatio` | 20 to 150 % |
| Response Curve | `in_gamepadLookCurve` | 1 to 4 (no number shown) |
| Dead Zone % | `in_gamepadDeadzone` | 0 to 50 % |
| Trigger Point % | `in_gamepadTrigger` | 5 to 95 % |
| Default Buttons | `in_gamepadDefaultBinds` | No, Yes |

## Ultrawide screens

Not a menu row: with `r_aspectRatio auto` (the default), retail snaps every
screen wider than 16:10 to 16:9, so a 21:9 or 32:9 screen showed a 16:9
picture stretched sideways. The port now uses the screen's own shape when it
is more than 5 % wider than 16:9 (`R_StoreWindowSettings`); screens near 16:9
(1366x768) still snap. The game keeps the vertical field of view, so the extra
width shows more at the sides. How the HUD and menus place themselves on such
a screen has not been looked at yet.

## How the pages work

A page (`kisak_options_display`, `kisak_options_controller`) is a copy of a
retail page (Options > Game, Controls > Look) with its own rows in place of the
retail ones and its own title. Each menu context (the main menu's, rebuilt by
`vid_restart`, and a map's in-game one) gets its own copy. Every tab that
leaves a port page closes it, and every retail page that gets a port tab closes
the port pages when it opens, as the retail pages close each other. Registering
a menu again (a second context, a `vid_restart`) adds no second row or tab.

The database can change a registered menu under the UI. Multiplayer has
the options menus twice, in `ui_mp.ff` and in `common_mp.ff`. Back at the
main menu after a map, `ui_mp.ff` loads again and, once it has finished
loading, copies its versions over `common_mp.ff`'s, which happens after the UI
has registered them. Without a fix, the tabs and the Game rows are gone at
that point. So every menu opened by name (`open`) gets the rows and tabs
back first (`UI_RefreshPortOptions`), and a port page whose retail page now
has other contents is built again. The rows moved down to make room are
copies, so the fast file's own items never move.

The memory the copies use is never freed: some tens of kilobytes per menu
context, and again for each menu the database has replaced.

None of this adds a dvar except `r_fullscreenMode`.

## Checking it without a game window

`KisakCOD-ui-asset-export` (see `docs/history/overnight-review-2026-10-31/ASSETS.md`)
loads `ui_mp.ff` with the real database and writes `menu-inventory.json`. With
a temporary block in `ExportMenus` that registers every menu in a fresh
`UiContext` with `UI_AppendMenuReference` and calls `UI_AddPortOptions` after
each, then adds the context's new menus to the export, the inventory shows the
rows, both pages and the tabs. Running the block twice checks that a second
registration adds nothing. The exporter registers no renderer or controller
dvars, so the block must register `r_backend`, `r_fullscreenMode` and the
`in_gamepad*` dvars first, or those rows (and the Controller page) are left
out. The tool reads multiplayer zones only, so the singleplayer menus have not
been checked this way.
