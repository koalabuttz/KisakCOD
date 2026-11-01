#include "ui_audio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace kisak::window {
void UiAudioMixer::Load(std::string_view name,std::span<const uint8_t> pcm,float volume) {
    if(name!="mouse_click" && name!="mouse_over")throw std::invalid_argument("Unsupported UI sound alias");
    if(pcm.empty() || pcm.size()%2 || pcm.size()>44100u*2*2 || !std::isfinite(volume) || volume<0 || volume>1)
        throw std::invalid_argument("UI sound requires bounded mono16 PCM and volume0..1");
    if(sounds_.contains(name))throw std::logic_error("UI sound already loaded");
    Sound sound; sound.volume=volume;sound.samples.reserve(pcm.size()/2);
    for(size_t i=0;i<pcm.size();i+=2) {
        const unsigned word=unsigned(pcm[i])|(unsigned(pcm[i+1])<<8);
        sound.samples.push_back(int16_t(word>=32768?int(word)-65536:int(word)));
    }
    sounds_.emplace(std::string(name),std::move(sound));
}
int UiAudioMixer::Play(std::string_view name) {
    const auto found=sounds_.find(name);if(found==sounds_.end())throw std::invalid_argument("UI sound is not loaded");
    if(nextId_==std::numeric_limits<int>::max())throw std::length_error("UI playback id exhausted");
    auto free=std::find_if(voices_.begin(),voices_.end(),[](auto &v){return !v.sound;});
    if(free==voices_.end())free=std::min_element(voices_.begin(),voices_.end(),[](auto &a,auto &b){return a.id<b.id;});
    *free={&found->second,0,nextId_};return nextId_++;
}
size_t UiAudioMixer::ActiveVoices() const noexcept {
    return std::count_if(voices_.begin(),voices_.end(),[](auto &v){return v.sound!=nullptr;});
}
void UiAudioMixer::Mix(std::span<int16_t> output) noexcept {
    for(auto &sample:output) {
        double sum=0;
        for(auto &voice:voices_)if(voice.sound) {
            sum+=voice.sound->samples[voice.position++]*double(voice.sound->volume);
            if(voice.position==voice.sound->samples.size())voice={};
        }
        sample=int16_t(std::round(std::clamp(sum,-32768.0,32767.0)));
    }
}
namespace {
std::runtime_error AudioError(const char *operation) {return std::runtime_error(std::string(operation)+": "+SDL_GetError());}
struct DeviceLock {
    SDL_AudioDeviceID device;
    explicit DeviceLock(SDL_AudioDeviceID id):device(id){SDL_LockAudioDevice(device);}
    ~DeviceLock(){SDL_UnlockAudioDevice(device);}
};
}
UiAudio::UiAudio():owner_(std::this_thread::get_id()) {
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)<0)throw AudioError("SDL audio initialization");
    struct InitGuard { bool retain=false;~InitGuard(){if(!retain)SDL_QuitSubSystem(SDL_INIT_AUDIO);} } init;
    SDL_AudioSpec wanted{},obtained{};
    wanted.freq=44100;wanted.format=AUDIO_S16SYS;wanted.channels=1;wanted.samples=512;
    wanted.callback=Callback;wanted.userdata=this;
    device_=SDL_OpenAudioDevice(nullptr,0,&wanted,&obtained,0);
    if(!device_)throw AudioError("SDL UI audio device open");
    if(obtained.freq!=44100 || obtained.format!=AUDIO_S16SYS || obtained.channels!=1) {
        SDL_CloseAudioDevice(device_);device_=0;
        throw std::runtime_error("SDL UI audio device returned unsupported format");
    }
    SDL_PauseAudioDevice(device_,0);init.retain=true;
}
UiAudio::~UiAudio() {
    // Close waits for an in-flight callback before owned sample storage dies.
    if(device_){SDL_CloseAudioDevice(device_);SDL_QuitSubSystem(SDL_INIT_AUDIO);}
}
void UiAudio::CheckThread() const {
    if(owner_!=std::this_thread::get_id())throw std::logic_error("UI audio requires owner thread");
}
void UiAudio::Load(std::string_view name,std::span<const uint8_t> pcm,float volume) {
    CheckThread();DeviceLock lock(device_);mixer_.Load(name,pcm,volume);
}
int UiAudio::Play(std::string_view name) {
    CheckThread();
    if(SDL_GetAudioDeviceStatus(device_)!=SDL_AUDIO_PLAYING)throw std::runtime_error("UI audio device is not playing");
    DeviceLock lock(device_);return mixer_.Play(name);
}
size_t UiAudio::ActiveVoices() {CheckThread();DeviceLock lock(device_);return mixer_.ActiveVoices();}
void UiAudio::Callback(void *user,Uint8 *stream,int bytes) noexcept {
    if(bytes<=0)return;
    if(bytes%2){std::memset(stream,0,size_t(bytes));return;}
    static_cast<UiAudio*>(user)->mixer_.Mix({reinterpret_cast<int16_t*>(stream),size_t(bytes)/2});
}
}
