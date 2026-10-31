#pragma once
#include <gfx_d3d/r_font.h>
#include <span>
#include <string>
#include <vector>
namespace kisak::window {
struct FontData { std::string name,materialName;int pixelHeight;std::vector<Glyph> glyphs; };
// The existing R_LoadFont loose layout: four32-bit header values, Glyph array,
// and bounded NUL-terminated names. Font/material ownership remains explicit.
FontData DecodeFont(std::span<const uint8_t> bytes);
FontData LoadFontFile(const std::string &path);
}
