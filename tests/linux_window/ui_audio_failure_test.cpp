// Actual production audio under allocation/device pressure. SDL dummy/disk
// exercise callbacks and output PCM, never physical speaker audibility.
#include <linux/client/window/ui_audio.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
using namespace kisak::window;
namespace {
thread_local int allocationBudget=-1;
int checks=0,failures=0;
void Check(bool ok,const char *message){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",message);}
template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
struct Fault {explicit Fault(int budget){allocationBudget=budget;}~Fault(){allocationBudget=-1;}};
std::vector<uint8_t> Pcm(size_t count,int16_t sample){std::vector<uint8_t> bytes(count*2);for(size_t i=0;i<count;++i){bytes[2*i]=uint16_t(sample)&255;bytes[2*i+1]=uint16_t(sample)>>8;}return bytes;}
bool Drain(UiAudio &audio){const auto start=SDL_GetTicks();while(audio.ActiveVoices() && SDL_GetTicks()-start<2000)SDL_Delay(1);return !audio.ActiveVoices();}
void *Allocate(size_t size){if(allocationBudget==0)throw std::bad_alloc();if(allocationBudget>0)--allocationBudget;if(void *p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
}
void *operator new(size_t n){return Allocate(n);}void *operator new[](size_t n){return Allocate(n);}
void operator delete(void *p) noexcept {std::free(p);}void operator delete[](void *p) noexcept {std::free(p);}
void operator delete(void *p,size_t) noexcept {std::free(p);}void operator delete[](void *p,size_t) noexcept {std::free(p);}

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
  SDL_setenv("SDL_AUDIODRIVER","disk",1);SDL_setenv("SDL_DISKAUDIOFILE",output.c_str(),1);SDL_setenv("SDL_DISKAUDIODELAY","1",1);
  auto pcm=Pcm(64,1000);for(size_t i=1;i<64;i+=2){pcm[i*2]=0x18;pcm[i*2+1]=0xfc;} // -1000, independently encoded.
  {
   UiAudio audio;Check(std::string_view(SDL_GetCurrentAudioDriver())=="disk","actual SDL disk output backend selected explicitly");
   audio.Load("mouse_click",pcm,.25f);Check(audio.Play("mouse_click")>0 && Drain(audio),"production callback renders admitted PCM to SDL output device");
  }
  const auto size=fs::file_size(output);if(!size || size>1024*1024 || size%2)throw std::runtime_error("Unexpected SDL disk output extent");
  std::vector<uint8_t> bytes(size);std::ifstream file(output,std::ios::binary);if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Cannot read complete callback output");
  size_t nonzero=0;bool exact=true;size_t previous=0;
  for(size_t i=0;i<bytes.size();i+=2){const unsigned word=unsigned(bytes[i])|(unsigned(bytes[i+1])<<8);const int sample=word>=32768?int(word)-65536:int(word);
   if(sample){exact&=sample==(nonzero%2?-250:250) && (!nonzero || i==previous+2);previous=i;++nonzero;}}
  Check(nonzero==64 && exact,"real SDL callback output contains exactly64 contiguous correctly scaled signed PCM samples with silence elsewhere");
  Check(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"disk device teardown releases SDL audio ownership");
  std::printf("disk_output_bytes=%zu nonzero_samples=%zu; synthetic PCM, NOT audibility\n",bytes.size(),nonzero);
 }else{
  SDL_setenv("SDL_AUDIODRIVER","dummy",1);
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
   Check(Reject([]{UiAudio unavailable;}),"actual second dummy device open fails explicitly");
   bool allocationFailure=false;try{Fault fault(0);UiAudio unavailable;}catch(const std::bad_alloc&){allocationFailure=true;}
   Check(allocationFailure,"allocation failure while reporting real device-open error propagates safely");
   Check(audio.Play("mouse_click")>0 && Drain(audio),"failed device constructors preserve existing live audio device");
  }
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
