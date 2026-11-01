#pragma once
#include "sdl_api.h"
#include <array>
#include <atomic>
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
// One playback device per instance. SDL2 writes the device buffer in place from
// its audio callback; SDL3 opens an audio stream on the default playback device
// and mixes into it from the stream's get callback, which pulls fixed chunks.
// Both kinds of callback are stopped before owned sample storage dies, and both
// report a device that stopped playing on the owner thread. A queue write the
// SDL3 device refuses loses PCM the mixer has already consumed, so that provider
// is failed for good and every owner call afterwards throws.
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
#if KISAK_LINUX_SDL_MAJOR == 3
    static void SDLCALL Callback(void *,SDL_AudioStream *,int,int) noexcept;
#else
    static void Callback(void *,Uint8 *,int) noexcept;
#endif
    void CheckThread() const;
    UiAudioMixer mixer_;
#if KISAK_LINUX_SDL_MAJOR == 3
    // Throws once the audio thread has lost a queue write. Called with the
    // stream lock held, which is the lock the callback itself runs under.
    void CheckFailure() const;
    // The stream owns its playback device: destroying it also closes the device.
    SDL_AudioStream *stream_=nullptr;
    // Callback scratch, sized once here so the audio thread never allocates.
    std::array<int16_t,1024> chunk_{};
    // Raised by the audio thread and never cleared: the mixer has already
    // consumed the PCM it handed to a refused queue write, so the provider
    // cannot report success afterwards.
    std::atomic<bool> failed_{false};
#else
    SDL_AudioDeviceID device_=0;
#endif
    std::thread::id owner_;
};
}
