#pragma once
#include <ui/ui_shared.h>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <cstdint>
namespace kisak::window {
// Stable heap ownership: engine pointers reference these arrays/strings.
struct MenuControlAsset {
    menuDef_t menu{};
    std::array<itemDef_s,4> items{};
    std::array<int,4> sourceIndices{};
    std::array<itemDef_s*,4> itemPointers{};
    std::string source;
    std::array<std::string,4> translationKeys,translations;
    std::array<std::string,4> menuStrings;
    std::array<std::array<std::string,5>,4> itemStrings;
    std::array<std::array<expressionEntry,2>,4> expressionEntries{};
    std::array<std::array<expressionEntry*,2>,4> expressionPointers{};
    MenuControlAsset()=default;
    MenuControlAsset(const MenuControlAsset&)=delete;
    MenuControlAsset& operator=(const MenuControlAsset&)=delete;
    const char *Localize(const char *key) const;
};
std::unique_ptr<MenuControlAsset> DecodeMenuControl(std::span<const uint8_t> bytes);
std::unique_ptr<MenuControlAsset> LoadMenuControlAsset(const std::string &bundle);
// Admission only. Does not execute or rewrite the original script.
bool MenuControlScriptSupported(const std::string &script);
}
