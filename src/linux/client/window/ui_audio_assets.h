#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace kisak::window {
class UiAudio;
struct UiSoundAsset { std::vector<uint8_t> pcm;float volume; };
// KUASND01 admits only the exported single-variant fixed-pitch UI profile.
UiSoundAsset DecodeUiSound(std::span<const uint8_t> bytes);
void LoadMenuAudio(UiAudio &,const std::string &bundle);
}
