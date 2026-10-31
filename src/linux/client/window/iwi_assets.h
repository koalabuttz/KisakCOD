#pragma once
#include "pixel_buffer.h"
#include <span>
#include <string>

namespace kisak::window {
// Owned RGBA pixels from the existing engine DXT decoder. Supported subset:
// IWI v6, 2D power-of-two DXT1/3/5, complete stored mip chain, top level >=4x4.
// Other formats/features reject explicitly. Caller supplies authorized bytes;
// no filesystem search, archive extraction, or asset fallback occurs here.
PixelBuffer DecodeIwi(std::span<const uint8_t> bytes);
PixelBuffer LoadIwiFile(const std::string &path);
}
