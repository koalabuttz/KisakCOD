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
#if KISAK_LINUX_SDL_MAJOR == 3
// SDL holds the stream lock while a get callback runs, so taking it here keeps
// the mixer exclusive of the audio thread. Device queries stay outside it: SDL
// documents device functions as taking this same lock.
struct StreamLock {
    SDL_AudioStream *stream;
    explicit StreamLock(SDL_AudioStream *held):stream(held){if(!SDL_LockAudioStream(stream))throw AudioError("SDL UI audio stream lock");}
    ~StreamLock(){SDL_UnlockAudioStream(stream);}
};
#else
struct DeviceLock {
    SDL_AudioDeviceID device;
    explicit DeviceLock(SDL_AudioDeviceID id):device(id){SDL_LockAudioDevice(device);}
    ~DeviceLock(){SDL_UnlockAudioDevice(device);}
};
#endif
}
UiAudio::UiAudio():owner_(std::this_thread::get_id()) {
#if KISAK_LINUX_SDL_MAJOR == 3
    if(!sdl::Ok(SDL_InitSubSystem(SDL_INIT_AUDIO)))throw AudioError("SDL audio initialization");
    // Owns the acquired subsystem reference and the stream until the device is
    // fully admitted. Open, format and resume failures all unwind here, so a
    // rejected device leaves neither a stream nor a subsystem reference behind.
    struct DeviceGuard {
        UiAudio &audio;bool retain=false;
        ~DeviceGuard(){
            if(retain)return;
            if(audio.stream_){SDL_DestroyAudioStream(audio.stream_);audio.stream_=nullptr;}
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    } guard{*this};
    // The stream carries the admitted playback format, so SDL converts it to
    // whatever the chosen device actually runs.
    const SDL_AudioSpec wanted{SDL_AUDIO_S16,1,44100};
    stream_=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&wanted,Callback,this);
    if(!stream_)throw AudioError("SDL UI audio device open");
    SDL_AudioSpec source{},destination{};
    if(!SDL_GetAudioStreamFormat(stream_,&source,&destination) ||
       source.format!=SDL_AUDIO_S16 || source.channels!=1 || source.freq!=44100)
        throw std::runtime_error("SDL UI audio device returned unsupported format");
    // The device is opened paused, so this is where playback starts.
    if(!SDL_ResumeAudioStreamDevice(stream_))throw AudioError("SDL UI audio device resume");
    guard.retain=true;
#else
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
#endif
}
UiAudio::~UiAudio() {
#if KISAK_LINUX_SDL_MAJOR == 3
    if(stream_) {
        // Disabling the get callback takes the stream lock, so an in-flight
        // callback has finished by the time it returns and none can start
        // after. The stream is then destroyed with no lock held, while the
        // mixer it mixed from is still alive: both outlive this body.
        SDL_SetAudioStreamGetCallback(stream_,nullptr,nullptr);
        SDL_DestroyAudioStream(stream_);
        stream_=nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#else
    // Close waits for an in-flight callback before owned sample storage dies.
    if(device_){SDL_CloseAudioDevice(device_);SDL_QuitSubSystem(SDL_INIT_AUDIO);}
#endif
}
void UiAudio::CheckThread() const {
    if(owner_!=std::this_thread::get_id())throw std::logic_error("UI audio requires owner thread");
}
#if KISAK_LINUX_SDL_MAJOR == 3
void UiAudio::CheckFailure() const {
    // Called with the stream lock held, so a refusal recorded by the callback
    // that just finished is visible here.
    if(failed_.load(std::memory_order_relaxed))
        throw std::runtime_error("UI audio provider failed: the device refused queued PCM");
}
#endif
void UiAudio::Load(std::string_view name,std::span<const uint8_t> pcm,float volume) {
#if KISAK_LINUX_SDL_MAJOR == 3
    CheckThread();StreamLock lock(stream_);CheckFailure();mixer_.Load(name,pcm,volume);
#else
    CheckThread();DeviceLock lock(device_);mixer_.Load(name,pcm,volume);
#endif
}
int UiAudio::Play(std::string_view name) {
    CheckThread();
#if KISAK_LINUX_SDL_MAJOR == 3
    // The device starts paused and SDL pauses it again once it stops, so this is
    // the stream-level form of the SDL2 status query. It stays outside the lock:
    // device-side state is not queried while the stream is being held.
    if(SDL_AudioStreamDevicePaused(stream_))throw std::runtime_error("UI audio device is not playing");
    StreamLock lock(stream_);CheckFailure();return mixer_.Play(name);
#else
    if(SDL_GetAudioDeviceStatus(device_)!=SDL_AUDIO_PLAYING)throw std::runtime_error("UI audio device is not playing");
    DeviceLock lock(device_);return mixer_.Play(name);
#endif
}
size_t UiAudio::ActiveVoices() {
#if KISAK_LINUX_SDL_MAJOR == 3
    // A failed provider must not look drained: reporting zero would read as a
    // completed sound rather than a lost one.
    CheckThread();StreamLock lock(stream_);CheckFailure();return mixer_.ActiveVoices();
#else
    CheckThread();DeviceLock lock(device_);return mixer_.ActiveVoices();
#endif
}
#if KISAK_LINUX_SDL_MAJOR == 3
void SDLCALL UiAudio::Callback(void *user,SDL_AudioStream *stream,int additional,int total) noexcept {
    (void)total;
    // SDL may ask for nothing, and overestimates the need for safety.
    if(additional<=0)return;
    UiAudio &audio=*static_cast<UiAudio*>(user);
    // A provider that already lost PCM stays quiet: mixing again would consume
    // more of the sound into a queue that cannot take it.
    if(audio.failed_.load(std::memory_order_relaxed))return;
    for(int remaining=additional;remaining>0;) {
        // Round the demand down to whole S16 frames. A request that ends
        // mid-frame is not padded into the queue: that would shift every later
        // frame by one byte.
        int chunk=std::min<int>(remaining,int(audio.chunk_.size()*sizeof(int16_t)));
        chunk-=chunk%int(sizeof(int16_t));
        if(chunk<=0)break;
        audio.mixer_.Mix({audio.chunk_.data(),size_t(chunk)/sizeof(int16_t)});
        if(!SDL_PutAudioStreamData(stream,audio.chunk_.data(),chunk)) {
            // The mixer has already advanced over these samples, so the sound
            // can never be completed. Fail the provider instead of retrying:
            // no device call and no throw from the audio thread.
            audio.failed_.store(true,std::memory_order_relaxed);
            return;
        }
        remaining-=chunk;
    }
}
#else
void UiAudio::Callback(void *user,Uint8 *stream,int bytes) noexcept {
    if(bytes<=0)return;
    if(bytes%2){std::memset(stream,0,size_t(bytes));return;}
    static_cast<UiAudio*>(user)->mixer_.Mix({reinterpret_cast<int16_t*>(stream),size_t(bytes)/2});
}
#endif
}
