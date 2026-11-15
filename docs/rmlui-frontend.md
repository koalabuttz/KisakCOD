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
| `ui_rmlStats` | Prints the last classic frame: slots, how many the stock painter drew, draw commands, vertices, indices. |

`ui_rml` takes effect on the next frame and is saved in the config. A server
cannot set it (`src/qcommon/server_dvar_guard.h`).

## How it fits together

```
UI_Refresh (ui_main_mp.cpp)
  ui_rml 0 -> Menu_PaintAll                    stock painter
  ui_rml 1 -> RmlFrontend_PaintClassic          src/ui_rml/rml_frontend.cpp
                ClassicPaint                    rml_classic.cpp: the menus as one RmlUi document
                Rml::Context::Update / Render
                  RenderInterface               rml_render.cpp -> R_AddCmdDrawUiTriangles
                  FontEngineInterface           rml_font.cpp   -> the game's Font_s glyphs
                  FileInterface                 rml_system.cpp -> FS_ReadFile
```

**Drawing.** RmlUi hands over textured, coloured, indexed triangle meshes. Each
becomes one `RC_DRAW_UI_TRIANGLES` render command
(`R_AddCmdDrawUiTriangles`, `src/gfx_d3d/r_rendercmds.cpp`), which the back end
draws like a stretch pic: the 2D pass, `TECHNIQUE_UNLIT`, the material's own
blend state, an optional scissor rectangle. The command sits in the shared
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

## Tests

`KisakCOD-rmlui-test` (`tests/rmlui/rmlui_frontend_test.cpp`) links the font
engine and the render interface against RmlUi and stand-ins for the engine
calls they make. It lays out a picture, a shadowed text with a colour code and
a fill the way the classic mode does, and checks the draw commands: document
order, rectangles, flips, tints, and every glyph's position, shadow and colour
against the stock text path's arithmetic. The x86_64 client job runs it after
the default build (`.github/workflows/build-kisakcod-linux.yaml`).

## Next

The classic mode is the proof. The plan continues with new menus written in
RML for every platform, starting with a bot-match screen, then the main menu
and settings; the stock menus stay available behind `ui_rml 0`.
