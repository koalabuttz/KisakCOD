// Actual production audio under allocation/device pressure. SDL dummy/disk
// exercise callbacks and output PCM, never physical speaker audibility.
#include <linux/client/window/ui_audio.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace kisak::window;
namespace {
thread_local int allocationBudget=-1;
int checks=0,failures=0;
// SDL2 spells the environment setter SDL_setenv; SDL3 renamed it.
#if KISAK_LINUX_SDL_MAJOR == 3
void SetEnv(const char *name,const char *value){
 if(std::strcmp(name,"SDL_AUDIODRIVER")==0)name="SDL_AUDIO_DRIVER";
 (void)SDL_setenv_unsafe(name,value,1);
}
#else
void SetEnv(const char *name,const char *value){SDL_setenv(name,value,1);}
#endif
void Check(bool ok,const char *message){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",message);}
template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
// Distinguishes the provider's own refusal from an unrelated failure: the owner
// must see the lost sound, not just any exception.
template<class F>bool FailsWith(F f,std::string_view needle){try{f();return false;}catch(const std::exception &e){return std::string_view(e.what()).find(needle)!=std::string_view::npos;}}
#if KISAK_LINUX_SDL_MAJOR == 3
// Waits for the audio thread to report the failure through the owner API.
bool AwaitFailure(UiAudio &audio){const auto start=SDL_GetTicks();while(SDL_GetTicks()-start<2000){try{audio.ActiveVoices();}catch(const std::exception&){return true;}SDL_Delay(1);}return false;}
// Queue-write injection. The target links with -Wl,--wrap=SDL_PutAudioStreamData,
// so every write from the audio thread passes through here first and production
// code keeps calling SDL_PutAudioStreamData unchanged.
std::atomic<int> putBudget{-1}; // -1 passes through, 0 refuses, n>0 allows n writes
std::atomic<long long> putBytes{0};
std::atomic<int> putCalls{0};
std::atomic<int> putRefused{0};
std::atomic<int> putOdd{0};
#endif
struct Fault {explicit Fault(int budget){allocationBudget=budget;}~Fault(){allocationBudget=-1;}};
std::vector<uint8_t> Pcm(size_t count,int16_t sample){std::vector<uint8_t> bytes(count*2);for(size_t i=0;i<count;++i){bytes[2*i]=uint16_t(sample)&255;bytes[2*i+1]=uint16_t(sample)>>8;}return bytes;}
bool Drain(UiAudio &audio){const auto start=SDL_GetTicks();while(audio.ActiveVoices() && SDL_GetTicks()-start<2000)SDL_Delay(1);return !audio.ActiveVoices();}
void *Allocate(size_t size){if(allocationBudget==0)throw std::bad_alloc();if(allocationBudget>0)--allocationBudget;if(void *p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
}
void *operator new(size_t n){return Allocate(n);}void *operator new[](size_t n){return Allocate(n);}
void operator delete(void *p) noexcept {std::free(p);}void operator delete[](void *p) noexcept {std::free(p);}
void operator delete(void *p,size_t) noexcept {std::free(p);}void operator delete[](void *p,size_t) noexcept {std::free(p);}
#if KISAK_LINUX_SDL_MAJOR == 3
extern "C" bool SDLCALL __real_SDL_PutAudioStreamData(SDL_AudioStream *,const void *,int);
extern "C" bool SDLCALL __wrap_SDL_PutAudioStreamData(SDL_AudioStream *stream,const void *data,int length) {
    putCalls.fetch_add(1);
    if(length%2)putOdd.fetch_add(1);
    const int budget=putBudget.load();
    if(budget==0){putRefused.fetch_add(1);return false;}
    if(budget>0)putBudget.fetch_sub(1);
    const bool queued=__real_SDL_PutAudioStreamData(stream,data,length);
    if(queued)putBytes.fetch_add(length);
    return queued;
}
#endif

int main(int argc,char **argv){try{
 const auto shortPcm=Pcm(8,2000);
 if((argc==2 && std::string_view(argv[1])=="--disk") || (argc==3 && std::string_view(argv[1])=="--disk-dir")) {
  namespace fs=std::filesystem;
  bool available=false;for(int i=0;i<SDL_GetNumAudioDrivers();++i)available|=std::string_view(SDL_GetAudioDriver(i))=="disk";
  if(!available)throw std::runtime_error("SDL disk diagnostic backend unavailable");
  const auto directory=argc==3?fs::path(argv[2]):fs::temp_directory_path()/("kisak-ui-audio-disk-"+std::to_string(SDL_GetPerformanceCounter()));
  if(!directory.is_absolute())throw std::runtime_error("Disk output directory must be absolute");
  if(!fs::create_directory(directory))throw std::runtime_error("Cannot create private output directory");
  struct Cleanup {fs::path directory;bool preserve;~Cleanup(){if(!preserve){std::error_code error;fs::remove_all(directory,error);}}} cleanup{directory,argc==3};
  fs::permissions(directory,fs::perms::owner_all);const auto output=directory/"callback.pcm";
  SetEnv("SDL_AUDIODRIVER","disk");
#if KISAK_LINUX_SDL_MAJOR == 3
  SetEnv(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,output.c_str());SetEnv(SDL_HINT_AUDIO_DISK_TIMESCALE,"1");
#else
  SetEnv("SDL_DISKAUDIOFILE",output.c_str());SetEnv("SDL_DISKAUDIODELAY","1");
#endif
  auto pcm=Pcm(64,1000);for(size_t i=1;i<64;i+=2){pcm[i*2]=0x18;pcm[i*2+1]=0xfc;} // -1000, independently encoded.
  // The admitted sound is mono. A device with more channels receives one copy of
  // each sample per channel, so the expected shape follows the device format.
  size_t channels=1;
  {
   UiAudio audio;Check(std::string_view(SDL_GetCurrentAudioDriver())=="disk","actual SDL disk output backend selected explicitly");
#if KISAK_LINUX_SDL_MAJOR == 3
   SDL_AudioSpec device{};int deviceFrames=0;
   if(!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&device,&deviceFrames))throw std::runtime_error("Cannot query the SDL disk device format");
   if(SDL_AUDIO_BITSIZE(device.format)!=16 || device.channels<1)throw std::runtime_error("SDL disk device is not signed16 with channels");
   channels=size_t(device.channels);
#endif
   audio.Load("mouse_click",pcm,.25f);Check(audio.Play("mouse_click")>0 && Drain(audio),"production callback renders admitted PCM to SDL output device");
  }
  const auto size=fs::file_size(output);if(!size || size>1024*1024 || size%2)throw std::runtime_error("Unexpected SDL disk output extent");
  std::vector<uint8_t> bytes(size);std::ifstream file(output,std::ios::binary);if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Cannot read complete callback output");
  size_t nonzero=0;bool exact=true,contiguous=true;size_t previous=0;
  for(size_t i=0;i<bytes.size();i+=2){const unsigned word=unsigned(bytes[i])|(unsigned(bytes[i+1])<<8);const int sample=word>=32768?int(word)-65536:int(word);
   if(sample){exact&=sample==((nonzero/channels)%2?-250:250);contiguous&=(!nonzero || i==previous+2);previous=i;++nonzero;}}
  Check(nonzero==64*channels && exact && contiguous,"real SDL callback output contains exactly64 contiguous correctly scaled signed PCM frames with silence elsewhere");
  Check(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"disk device teardown releases SDL audio ownership");
  std::printf("disk_output_bytes=%zu nonzero_samples=%zu channels=%zu; synthetic PCM, NOT audibility\n",bytes.size(),nonzero,channels);
 }else{
  SetEnv("SDL_AUDIODRIVER","dummy");
  std::puts("Fault injection targets C++ allocations only; SDL device is actual dummy, NOT audibility.");
  unsigned faults=0;bool reachedSuccess=false;
  for(int budget=0;budget<8 && !reachedSuccess;++budget){
   UiAudioMixer mixer;mixer.Load("mouse_click",shortPcm,.5f);mixer.Play("mouse_click");bool failed=false;
   try{Fault fault(budget);mixer.Load("mouse_over",shortPcm,1);}catch(const std::bad_alloc&){failed=true;}
   if(failed){++faults;Check(Reject([&]{mixer.Play("mouse_over");}),"failed resource allocation publishes no alias");
    std::array<int16_t,8> samples{};mixer.Mix(samples);bool intact=true;for(auto sample:samples)intact&=sample==1000;
    Check(intact,"allocation failure preserves existing owned resource and pending voice");
    mixer.Load("mouse_over",shortPcm,1);Check(mixer.Play("mouse_over")>0,"resource load retries successfully after allocation failure");
   }else reachedSuccess=true;
  }
  Check(faults>=2 && reachedSuccess,"fault sweep covers PCM allocation and map-node publication before success");
  {
   UiAudioMixer mixer;mixer.Load("mouse_click",shortPcm,.5f);std::array<int16_t,8> samples{};bool stable=true;
   {Fault fault(0);for(int n=0;n<1000;++n)stable&=mixer.Play("mouse_click")>0;mixer.Mix(samples);}
   for(auto sample:samples)stable&=sample==8000;
   Check(stable && mixer.ActiveVoices()==0,"1000-repeat capacity replacement and actual bounded mixer path require no C++ allocation");
  }
  {
   UiAudio audio;bool failed=false;try{Fault fault(0);audio.Load("mouse_click",shortPcm,1);}catch(const std::bad_alloc&){failed=true;}
   Check(failed,"actual device-owned resource load propagates allocation failure");
   audio.Load("mouse_click",shortPcm,1);Check(audio.Play("mouse_click")>0 && Drain(audio),"failed load releases device lock and callback continues after retry");
#if KISAK_LINUX_SDL_MAJOR == 3
   // SDL3 gives each stream its own logical device, so a second provider coexists
   // where SDL2's dummy backend refused the second open.
   {UiAudio second;second.Load("mouse_click",shortPcm,1);
    Check(second.Play("mouse_click")>0 && Drain(second),"SDL3 admits a second provider on the default device instead of failing the open");}
#else
   Check(Reject([]{UiAudio unavailable;}),"actual second dummy device open fails explicitly");
   bool allocationFailure=false;try{Fault fault(0);UiAudio unavailable;}catch(const std::bad_alloc&){allocationFailure=true;}
   Check(allocationFailure,"allocation failure while reporting real device-open error propagates safely");
#endif
   Check(audio.Play("mouse_click")>0 && Drain(audio),"failed device constructors preserve existing live audio device");
  }
#if KISAK_LINUX_SDL_MAJOR == 3
  {
   // Deterministic device-open failure with the audio subsystem already
   // initialized: the disk backend has a usable driver but refuses this output
   // path, so the open itself is what fails. This unwinds through the same
   // rollback a failed resume takes, and needs no hook in production code.
   bool disk=false;for(int i=0;i<SDL_GetNumAudioDrivers();++i)disk|=std::string_view(SDL_GetAudioDriver(i))=="disk";
   if(!disk)throw std::runtime_error("SDL disk diagnostic backend unavailable");
   SetEnv("SDL_AUDIODRIVER","disk");SetEnv(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,"/proc/kisak-ui-audio-no-such-directory/callback.pcm");
   Check(Reject([]{UiAudio unavailable;}),"deterministic SDL device-open failure throws instead of reporting playback success");
   Check(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"failed device open returns the acquired SDL audio subsystem reference");
   bool allocationFailure=false;try{Fault fault(0);UiAudio unavailable;}catch(const std::bad_alloc&){allocationFailure=true;}
   Check(allocationFailure && !(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"allocation failure while reporting a real device-open error still returns the subsystem reference");
   SetEnv("SDL_AUDIODRIVER","dummy");
   {UiAudio recovered;recovered.Load("mouse_click",shortPcm,1);
    Check(recovered.Play("mouse_click")>0 && Drain(recovered),"rollback leaves SDL audio able to open the next device");}
  }
#endif
#if KISAK_LINUX_SDL_MAJOR == 3
  {
   // The device queue is the one place the provider can still lose PCM after the
   // mixer has consumed it. These cases refuse that write through the linker wrap
   // and require the owner to see the loss rather than a drained sound. The target
   // must link with -Wl,--wrap=SDL_PutAudioStreamData: without it the __real_
   // reference below does not resolve, so the flag cannot be silently dropped.
   putBudget=-1;
   {UiAudio audio;audio.Load("mouse_click",shortPcm,1);
    putRefused=0;putBudget=0; // the first write for this sound is refused
    const bool surfaced=AwaitFailure(audio);
    Check(surfaced && putRefused>0,"first refused queue write surfaces as an owner-visible error");
    Check(FailsWith([&]{audio.Play("mouse_click");},"provider failed"),"failed provider rejects subsequent playback");
    Check(FailsWith([&]{audio.Load("mouse_over",shortPcm,1);},"provider failed"),"failed provider rejects further resource loads");
    Check(FailsWith([&]{audio.ActiveVoices();},"provider failed"),"failed provider never reports a drained mixer");}
   putBudget=-1;
   Check(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"provider failed before any sound still tears its device down");
   {const auto longPcm=Pcm(88200,1234);
    putRefused=0;putBudget=-1;putBytes=0;
    UiAudio audio;audio.Load("mouse_click",longPcm,.25f);
    const long long bytesBefore=putBytes.load();
    putBudget=4; // a few writes land, then one is refused mid-sound
    const int id=audio.Play("mouse_click");
    const size_t before=audio.ActiveVoices();
    const bool surfaced=AwaitFailure(audio);
    Check(id>0 && before==1 && surfaced && putBytes.load()>bytesBefore,"refused queue write after queued PCM surfaces instead of draining the voice");
    Check(putOdd==0,"provider queues only whole S16 frames, never a padded byte");
    Check(FailsWith([&]{audio.ActiveVoices();},"provider failed"),"lost sound is reported as a failure rather than as completed playback");
    Check(FailsWith([&]{audio.Play("mouse_click");},"provider failed"),"partially played provider rejects later playback");}
   putBudget=-1;
   Check(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"provider failed mid-sound still tears its device down");
   std::printf("queue_injection calls=%d refused=%d bytes=%lld; synthetic PCM, NOT audibility\n",putCalls.load(),putRefused.load(),putBytes.load());
  }
#endif
  Check(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"normal and bad_alloc device-open failures leak no SDL subsystem reference");
  const auto longPcm=Pcm(88200,1234);bool lifecycle=true;
  for(int cycle=0;cycle<8;++cycle){
   {UiAudio audio;audio.Load("mouse_click",longPcm,.25f);int previous=0;for(int n=0;n<1000;++n){const auto id=audio.Play("mouse_click");lifecycle&=id>previous;previous=id;}lifecycle&=audio.ActiveVoices()==8;}
   lifecycle&=!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO);
  }
  Check(lifecycle,"eight actual callback device restarts close safely with eight pending long voices after rapid repeat");
 }
 }catch(const std::exception&e){allocationBudget=-1;std::fprintf(stderr,"audio failure fixture: %s\n",e.what());++failures;}
 std::printf("UI audio failure: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
