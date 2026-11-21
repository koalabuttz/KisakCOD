#pragma once

// Shared between the RmlUi front-end's units (src/ui_rml). Engine types are
// only forward-declared here, so a unit can include RmlUi and this header
// without the engine's headers.

#include <RmlUi/Core.h>

#include <cstddef>
#include <cstdint>

struct Material;
struct Font_s;
struct UiContext;

namespace kisak_rml
{
// Texture sources the render interface understands (RenderInterface::LoadTexture):
//   "material:<name>"  a game material, registered by name
//   "font:<name>"      a game font's glyph material (R_RegisterFont)
// A texture handle is the Material pointer itself.
constexpr const char *kMaterialSource = "material:";
constexpr const char *kFontSource = "font:";

Material *MaterialFromTexture(Rml::TextureHandle texture);

// The engine-side interfaces RmlUi is given (rml_render.cpp, rml_font.cpp,
// rml_system.cpp). Each lives for the whole process once made.
Rml::RenderInterface *RenderInterface();
// Drops the cached engine materials (the UI is shutting down).
void RenderForget();
Rml::FontEngineInterface *FontEngine();
Rml::SystemInterface *SystemInterface();
Rml::FileInterface *FileInterface();

// The game font a family name stands for, and the exact scale encoded in it.
// RmlUi hands font sizes over as whole pixels; the classic mode needs the
// stock scale to the last fraction, so a family may carry it:
//   "fonts/normalFont"              size from font-size, scale = size / pixelHeight
//   "fonts/normalFont@0.43125"      glyph scale 0.43125, font-size ignored
//   "fonts/normalFont@0.45x0.43"    horizontal and vertical glyph scales
// FontFamilyFor builds the scaled forms (rml_font.cpp).
Rml::String FontFamilyFor(const Font_s *font, float xScale, float yScale);

// The ascent the font engine reports for a font at a vertical glyph scale:
// whole pixels, so a baseline placed from it stays on a pixel row.
float FontAscent(const Font_s *font, float yScale);

// The classic mode (rml_classic.cpp): the stock menus as one RmlUi document.
// ClassicRegister runs once, before any document; ClassicPaint builds this
// frame's document in the context, ClassicHide hides every slot (the stock
// painter draws this frame), ClassicRelease forgets it (the context
// owns it and is about to drop it).
void ClassicRegister();
void ClassicPaint(UiContext *dc, Rml::Context *context);
void ClassicHide();
void ClassicDump(); // the last frame's slots, to the console
void ClassicRelease();

// The screen files built into the game (src/ui_rml/screens, embedded by
// scripts/extern/rmlui.cmake): "ui_rml/<file>". The file interface serves
// them when the game folders have no file of that name, so a loose copy in
// the game folder overrides the built-in one.
bool EmbeddedFile(const char *path, const char **data, size_t *size);

// Frame counters for ui_rmlStats.
struct FrameStats
{
    int commands;
    int vertices;
    int indices;
    int slots;
    int fallbacks;
    int untextured;      // fills, borders, gradients: drawn with the white material
    const char *white;   // that material's name, as registered
};
FrameStats &Stats();
} // namespace kisak_rml
