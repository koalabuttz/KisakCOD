#pragma once
#include <cstdint>
#include <span>
#include <string>
namespace kisak::window {
// Resolved only after validating the complete exported technique/state/sampler
// description. Fingerprints identify supported local programs, not authenticity.
struct ResolvedUiMaterial {
    std::string name;
    std::string imageName;
    bool strictSampler=true;
    unsigned samplerState=235;
};
ResolvedUiMaterial DecodeUiCompositionMaterial(std::span<const uint8_t> bytes);
ResolvedUiMaterial LoadUiCompositionMaterialFile(const std::string &path);
ResolvedUiMaterial DecodeUiMaterial(std::span<const uint8_t> bytes);
ResolvedUiMaterial LoadUiMaterialFile(const std::string &path);
}
