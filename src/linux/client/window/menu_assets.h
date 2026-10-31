#pragma once
#include "cpu_renderer.h"
#include <ui/ui_shared.h>
#include <string>
#include <span>
namespace kisak::window {
struct MenuTextAsset { itemDef_s item{};std::string text,source;CpuRenderer::MenuFonts fonts; };
MenuTextAsset DecodeMenuText(std::span<const uint8_t>);
MenuTextAsset LoadMenuTextFile(const std::string &);
// Caller owns renderer lifetime; resources are released by its normal shutdown.
CpuRenderer::MenuFonts LoadMenuFonts(CpuRenderer &,const std::string &bundle,const std::string &imageDirectory);
MenuTextAsset LoadMenuBundle(CpuRenderer &,const std::string &bundle,const std::string &imageDirectory);
}
