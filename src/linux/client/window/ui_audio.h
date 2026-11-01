#pragma once
#include <SDL.h>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kisak::window {
// Admitted UI subset only: owned mono signed16 little-endian PCM at44100Hz,
// fixed pitch1, no chain/delay/variant. Caller validates exported alias metadata.
// At capacity the oldest of eight voices is replaced by the admitted playback.
// Mixer is thread-confined; UiAudio serializes it against the SDL callback.
class UiAudioMixer {
public:
    UiAudioMixer()=default;
    UiAudioMixer(const UiAudioMixer&)=delete;
    UiAudioMixer& operator=(const UiAudioMixer&)=delete;
    void Load(std::string_view name,std::span<const uint8_t> pcm,float volume);
    int Play(std::string_view name);
    void Mix(std::span<int16_t> output) noexcept;
    size_t ActiveVoices() const noexcept;
private:
    struct Sound { std::vector<int16_t> samples;float volume; };
    struct Voice { const Sound *sound=nullptr;size_t position=0;int id=0; };
    std::map<std::string,Sound,std::less<>> sounds_;
    std::array<Voice,8> voices_{};
    int nextId_=1;
};
class UiAudio {
public:
    UiAudio();
    ~UiAudio();
    UiAudio(const UiAudio&)=delete;
    UiAudio& operator=(const UiAudio&)=delete;
    void Load(std::string_view name,std::span<const uint8_t> pcm,float volume);
    int Play(std::string_view name);
    size_t ActiveVoices();
private:
    static void Callback(void *,Uint8 *,int) noexcept;
    void CheckThread() const;
    UiAudioMixer mixer_;
    SDL_AudioDeviceID device_=0;
    std::thread::id owner_;
};
}
