#pragma once
#include "cpu_renderer.h"
#include <ui/ui_shared.h>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <span>
namespace kisak::window {
struct MenuCompositionItem {
 itemDef_s item{};unsigned sourceIndex=0;CpuRenderer::UiMaterialHandle material=0;
 // Immutable after decode: the literal expression borrows its stable c_str().
 std::string materialName;
 std::array<expressionEntry,2> expressionEntries{};
 std::array<expressionEntry*,2> expressionPointers{};
 // Records stay at a stable address; statement pointers borrow these owned fields.
 MenuCompositionItem()=default;
 MenuCompositionItem(const MenuCompositionItem &)=delete;
 MenuCompositionItem &operator=(const MenuCompositionItem &)=delete;
};
struct MenuCompositionAssets {
 std::vector<std::unique_ptr<MenuCompositionItem>> items;
 CpuRenderer::UiMaterialHandle cursor=0;
 int fadeCycle=0;float fadeClamp=0,fadeAmount=0,fadeInAmount=0;
};
MenuCompositionAssets DecodeMenuComposition(std::span<const uint8_t>);
// Successful resources belong to the renderer generation; failures release all
// resources created by this call. No game assets are embedded in this loader.
MenuCompositionAssets LoadMenuComposition(CpuRenderer &,const std::string &bundle,const std::string &images);
}
