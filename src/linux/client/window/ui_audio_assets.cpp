#include "ui_audio_assets.h"
#include "ui_audio.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <stdexcept>
namespace kisak::window {
UiSoundAsset DecodeUiSound(std::span<const uint8_t> bytes) {
    if(bytes.size()<52 || bytes.size()>176452)throw std::invalid_argument("UI sound resource bounds");
    if(std::memcmp(bytes.data(),"KUASND01",8))throw std::invalid_argument("UI sound format");size_t offset=8;
    auto word=[&](){uint32_t out=uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|(uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);offset+=4;return out;};
    const auto flags=word(),format=word(),rate=word(),bits=word(),channels=word(),block=word(),pitchBits=word(),volumeBits=word(),samples=word(),dataLength=word(),delay=word();
    float pitch,volume;std::memcpy(&pitch,&pitchBits,4);std::memcpy(&volume,&volumeBits,4);
    if(flags!=2648||format!=1||rate!=44100||bits!=16||channels!=1||block!=2||pitch!=1||delay||!std::isfinite(volume)||volume<0||volume>1||!samples||samples>88200||dataLength!=samples*2||bytes.size()-offset!=dataLength)
        throw std::invalid_argument("Unsupported UI sound alias profile");
    return {{bytes.begin()+offset,bytes.end()},volume};
}
void LoadMenuAudio(UiAudio &audio,const std::string &bundle) {
    // Validate both resources before publishing either. An allocation/device
    // error during provider loading still propagates to the owning host.
    std::vector<UiSoundAsset> sounds;
    const char *aliases[]={"mouse_click","mouse_over"};
    for(const char *alias:aliases) {
        std::ifstream file(std::filesystem::path(bundle)/(std::string(alias)+".sound"),std::ios::binary|std::ios::ate);
        if(!file)throw std::runtime_error("Missing genuine UI sound resource");const auto length=file.tellg();
        if(length<52 || length>176452)throw std::invalid_argument("UI sound resource bounds");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));file.seekg(0);if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Cannot read UI sound");
        sounds.push_back(DecodeUiSound(bytes));
    }
    for(size_t i=0;i<sounds.size();++i)audio.Load(aliases[i],sounds[i].pcm,sounds[i].volume);
}
}
