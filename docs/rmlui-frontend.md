# RmlUi menu front-end

An opt-in front-end that draws menus with [RmlUi](https://github.com/mikke89/RmlUi)
(MIT licence), a C++ library that lays out and draws documents written in RML
and RCSS, look-alikes of HTML and CSS. It is the base for new menus that work
with mouse, controller and touch on every platform. The first step is the
**classic mode**: the stock menus, exactly as the fastfiles define them, drawn
by RmlUi instead of the stock painter. It proves that RmlUi draws the original
menus, with the game's own art and fonts, on every renderer, before any new
screen is written.

Nothing from the game is copied into the repository: every menu, material and
font comes from the player's own game files at run time.

## Building

```sh
cmake -B build ... -DKISAK_RMLUI=ON
```

- Windows: `KisakCOD-mp`.
- Linux, macOS, Android: the native client `KisakCOD-mp`
  (`-DKISAK_BUILD_LINUX_MP_CLIENT=ON`), plus the asset-free test
  `KisakCOD-rmlui-test`.

The option is off by default. CMake fetches RmlUi 6.3 at configure time
(`scripts/extern/rmlui.cmake`) and builds only its core library, as a static
library: no FreeType, Lua, SVG or Lottie. To build offline, point
`FETCHCONTENT_SOURCE_DIR_RMLUI` at a checkout of the same tag.

## Using it

| Console | Meaning |
| --- | --- |
| `ui_rml 0` | Default. The stock menu painter. |
| `ui_rml 1` | Classic mode: the stock menus drawn by RmlUi. |
| `ui_rmlStats` | Prints the last frame: classic slots and how many the stock painter drew, draw commands, vertices, indices; the open screen, and how many draws had no texture (plain fills, drawn with the `white` material) or a texture. `ui_rmlStats 1` also lists every classic slot (`RmlUi slot ...` lines): the window that made it and what it set. |
| `ui_rmlOpen <screen>` | Opens the screen `ui_rml/<screen>.rml` over the menus, e.g. `ui_rmlOpen graphics`. |
| `ui_rmlClose` | Closes it. |
| `ui_rmlGraphics 1` | Options > Graphics opens the graphics screen in place of the stock page. On by default on Android, off elsewhere. |

`ui_rml` takes effect on the next frame. `ui_rml` and `ui_rmlGraphics` are
saved in the config, and a server cannot set them
(`src/qcommon/server_dvar_guard.h`). Android builds take the front-end with
`build_apk.py --rmlui`.

## How it fits together

```
UI_Refresh (ui_main_mp.cpp) -> RmlFrontend_PaintMenus   src/ui_rml/rml_frontend.cpp
  ui_rml 0 -> Menu_PaintAll                    stock painter
  ui_rml 1 -> ClassicPaint                     rml_classic.cpp: the menus as one RmlUi document
  a screen open -> Screens::Refresh            rml_screens.cpp: rows brought up to date
  Rml::Context::Update / Render                (when classic mode or a screen is on)
                  RenderInterface               rml_render.cpp -> R_AddCmdDrawUiTriangles
                  FontEngineInterface           rml_font.cpp   -> the game's Font_s glyphs
                  FileInterface                 rml_system.cpp -> FS_ReadFile, then the built-in screens
UI_KeyEvent, UI_MouseEvent -> RmlFrontend_KeyEvent / MouseEvent (the open screen takes them)
Menus_OpenByName("options_graphics") -> UI_MenuOpenHook (ui_rmlGraphics: the graphics screen)
```

**Drawing.** RmlUi hands over textured, coloured, indexed triangle meshes. Each
becomes one `RC_DRAW_UI_TRIANGLES` render command
(`R_AddCmdDrawUiTriangles`, `src/gfx_d3d/r_rendercmds.cpp`), which the back end
draws like a stretch pic: the 2D pass, `TECHNIQUE_UNLIT`, the material's own
blend state, an optional scissor rectangle. Plain fills (backgrounds,
borders) use the `white` material, as `UI_FillRect` does. Every triangle is
turned the way `RB_DrawStretchPic` turns its own (clockwise on screen):
RmlUi builds its quads the other way, and some drivers (the Android
emulator's) then culled every fill and picture and drew only the text. The
command sits in the shared
back-end command table, so every back end that draws the stock menus (D3D9,
DXVK, Vulkan through its D3D9 front, GXM) runs it without code of its own.
Large meshes are split into commands of at most 1,500
vertices.

**Textures** are game materials, never uploaded by RmlUi:
`material:<name>` registers a material by name, `font:<name>` takes a font's
glyph material. RmlUi's own generated textures are refused (its font engine
and layer effects are not used).

**Fonts.** RmlUi is built without a font engine. `rml_font.cpp` lays text out
from the game's `Font_s` glyph tables and draws each glyph exactly where the
stock text path (`DrawText2D`, `rb_backend.cpp`) puts it, with the same colour
codes (`^0` to `^9`) and shadows (`font-effect: shadow(...)`). A font family
can carry the exact stock glyph scale, which RmlUi's whole-pixel font sizes
cannot: `fonts/normalFont@0.45` or `fonts/normalFont@0.45x0.43`. Without a
scale, `font-size` picks it.

## The classic mode

Every frame, `rml_classic.cpp` walks the open menus in `Menu_PaintAll`'s order
and turns each thing the stock painter would draw into a slot element of one
RmlUi document, in the same order:

| Stock drawing | Slot |
| --- | --- |
| Filled window (style 1 without a background), borders 1 to 3 | `background-color` |
| Window background (styles 1, 3, 5), full-screen menu background | `decorator: image("material:<name>" <flip>)`, `image-color` |
| Text and button items (types 0 and 1), text styles 0 to 6 | a text node in the game font at the stock scale, `color`, `font-effect: shadow(...)` |
| Anything else | the stock painter, called from the slot when RmlUi draws it |

Slots are placed in real screen pixels, the stock coordinates after
`ScrPlace_ApplyRect`, and each is a stacking context, so RmlUi draws them in
document order. Slots are pooled: a frame changes only what differs from the
last one.

The walk also does everything the stock painter does besides drawing, in the
same order: visibility, fades, menu sounds, blur, rect expressions, item
materials from dvars and expressions, text extents, and focus loss of hidden
items. Input, focus, scripts and sounds stay with the stock menu code, so the
menus behave exactly as before.

Still drawn by the stock painter (from inside RmlUi's draw order): list boxes,
sliders, edit fields, yes/no, multi and enum choices, key binds, owner draws,
game message windows, auto-wrapped and cinematic text, monospace text, text
with HUD icons, the load bar (window style 6), highlight borders (5 and 6) and
the debug outlines. `ui_rmlStats` counts them. `ui_showList 1` uses the stock
painter for the whole frame.

### Known differences

- RmlUi places boxes on whole pixels. A stock rectangle with fractional edges
  covers the same pixels, but its texture is stretched over the rounded
  rectangle, a fraction of a texel apart.
- RmlUi keeps colours premultiplied in bytes. A colour at very low alpha (the
  first frames of a fade) can come back one or two steps off.
- Text is read as Latin-1, which covers the Western languages. Languages the
  stock code reads as multi-byte strings are not handled yet.

## Screens

New menus are RML documents, `src/ui_rml/screens/<name>.rml`, with the shared
style sheet `kisak.rcss`. They are our own files, not game assets. CMake
builds them into the game (`scripts/extern/rmlui.cmake` generates
`rml_embedded.cpp`), so nothing needs copying beside the game; a file of the
same name in a game folder's `ui_rml/` (loose, or in an .iwd) overrides the
built-in copy, which lets a screen be changed on a device without a rebuild.
Sizes are in `dp`: one dp is a pixel of a 720-line screen, so a screen keeps
its proportions at any resolution. Fonts are the game's (`font-family:
"fonts/normalFont"`).

A screen holds no code. Its rows are bound to dvars by attributes
(`rml_screens.cpp` lists them all):

```xml
<page id="effects" title="Effects">
  <row dvar="sm_enable" type="toggle" label="Shadows" hint="Sun and spot light shadows."/>
  <row dvar="r_texFilterAnisoMax" type="choice" label="Anisotropic Filtering" options="Off=1|4x=4|16x=16"/>
  <row dvar="cg_fov" type="slider" label="Field of View" min="65" max="120" step="1"/>
  <row dvar="r_modernSunStrength" type="slider" min="0" max="4" step="0.1" auto="-1" save="1"/>
</page>
<div class="button" cmd="apply">APPLY</div>
```

- A row for a dvar the build lacks hides itself (the Vita's rows on Android,
  the Modern rows without the Vulkan back end). `show-if="r_visuals=modern"`
  shows a row or a page only while the condition holds.
- A dvar with a latch (`r_visuals`, `r_aaSamples`, `r_vsync`), or a row
  marked `restart="1"` (texture sizes), shows "Restart" once changed, and the
  Apply button appears; it runs `vid_restart`, and the screen opens again
  after it.
- `save="1"` writes the dvar to the config even when it is not saved by
  default (the `r_modern*` switches).
- `cmd` on any element runs on a click: `close`, `apply`, `page <id>`,
  `set` / `seta <dvar> <value>`, or any console command. The quality presets
  are rows of `set`s.

Input. The engine already turns every device into keys and a mouse, and an
open screen takes all of them:

| Input | Screen |
| --- | --- |
| Up, Down (D-pad, arrows) | the previous or next row shown |
| Left, Right | change the focused row (or the page, on a tab) |
| Enter, A, a tap or click on a row | step it forward; a tap on its left arrow steps back, a tap on a slider's bar sets it there |
| Bumpers, Page Up / Down | the previous or next page |
| Escape, B, right mouse button | close |
| A touch that moves | scrolls the page |

A slider's value is a fill along a faint track; a tap anywhere on the bar
sets it.

The graphics screen (`graphics.rml`) has six pages: Quality (Low, Medium,
High and Ultra presets), Display, Textures, Effects, Modern and Advanced (the
last two only with Modern visuals).

## Tests

`KisakCOD-rmlui-test` (`tests/rmlui/rmlui_frontend_test.cpp`) links the font
engine and the render interface against RmlUi and stand-ins for the engine
calls they make. It lays out a picture, a shadowed text with a colour code and
a fill the way the classic mode does, and checks the draw commands: document
order, rectangles, flips, tints, and every glyph's position, shadow and colour
against the stock text path's arithmetic. It then opens the built-in
graphics screen with stand-in dvars and plays it with keys and taps: hidden
rows, show-if, pages by bumper, choices, toggles, sliders (step, tap on the
bar, Auto), restart marks, a preset, Apply and Escape. It also checks that
every triangle drawn turns the stock way. The x86_64 client job runs it after
the default build (`.github/workflows/build-kisakcod-linux.yaml`).

## Next

The classic mode is the proof, and the graphics screen the first new menu.
Both draw correctly on the Retroid Pocket 6 and the Android emulator
(November 2026), in Legacy and Modern visuals. Next: a render resolution
scale (the Vulkan back end needs one first), then the bot-match screen, the main menu and
the rest of the settings; the stock menus stay available behind `ui_rml 0`
and `ui_rmlGraphics 0`.
