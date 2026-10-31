#include <linux/client/window/ui_audio.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <thread>
#include <stdexcept>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
template<class F> bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
std::vector<uint8_t> Pcm(std::initializer_list<int16_t> samples){std::vector<uint8_t> bytes;for(auto sample:samples){const auto word=uint16_t(sample);bytes.push_back(word&255);bytes.push_back(word>>8);}return bytes;}
}
int main(int argc,char **argv) {try {
 if(argc==2 && std::string_view(argv[1])=="--unavailable") {
  SDL_setenv("SDL_AUDIODRIVER","kisak_invalid_audio_driver",1);
  Check(Reject([]{UiAudio audio;}),"unavailable SDL audio driver throws instead of reporting playback success");
 } else {
  UiAudioMixer mixer;auto pcm=Pcm({1000,-1000,32767,-32768});mixer.Load("mouse_click",pcm,.5f);std::fill(pcm.begin(),pcm.end(),0);
  const int first=mixer.Play("mouse_click");std::array<int16_t,2> half{};mixer.Mix(half);
  Check(first>0 && half==std::array<int16_t,2>{500,-500} && mixer.ActiveVoices()==1,"owned little-endian PCM applies alias volume and advances across callback chunks");
  std::array<int16_t,4> tail{};mixer.Mix(tail);
  Check(tail==std::array<int16_t,4>{16384,-16384,0,0} && mixer.ActiveVoices()==0,"last partial chunk rounds signed samples and zero-fills after completion");
  mixer.Load("mouse_over",Pcm({32767,-32768}),1);const auto a=mixer.Play("mouse_over"),b=mixer.Play("mouse_over");mixer.Mix(half);
  Check(a>first && b>a && half==std::array<int16_t,2>{32767,-32768},"simultaneous actual mixer voices saturate both signed endpoints and have distinct ids");
  mixer.Play("mouse_over");for(int i=0;i<7;++i)mixer.Play("mouse_click");
  const int replacement=mixer.Play("mouse_click");mixer.Mix(half);
  Check(replacement>b && mixer.ActiveVoices()==8 && half==std::array<int16_t,2>{4000,-4000},"at eight voices a new playback replaces exactly the oldest voice and stays bounded");
  mixer.Mix(tail);Check(mixer.ActiveVoices()==0 && mixer.Play("mouse_click")>b,"completed voice capacity is reusable");
  Check(Reject([&]{mixer.Load("mouse_click",Pcm({1}),1);}),"loaded resource replacement rejects without dangling active voices");
  Check(Reject([&]{mixer.Play("unknown");}) && Reject([&]{mixer.Load("unknown",Pcm({1}),1);}),"unsupported aliases reject explicitly");
  UiAudioMixer invalid;
  Check(Reject([&]{invalid.Load("mouse_click",{},1);}) && Reject([&]{invalid.Load("mouse_click",std::array<uint8_t,1>{1},1);}),"empty and partial sample PCM reject");
  Check(Reject([&]{invalid.Load("mouse_click",Pcm({1}),-1);}) && Reject([&]{invalid.Load("mouse_click",Pcm({1}),std::numeric_limits<float>::quiet_NaN());}) && Reject([&]{invalid.Load("mouse_click",Pcm({1}),1.1f);}),"invalid volume rejects before resource publication");
  std::vector<uint8_t> oversized(44100*4+2);Check(Reject([&]{invalid.Load("mouse_click",oversized,1);}),"two-second per-alias resource bound enforced");
  SDL_setenv("SDL_AUDIODRIVER","dummy",1);
  std::puts("SDL dummy-driver checks verify callback/device lifecycle, NOT audibility.");
  for(int cycle=0;cycle<3;++cycle) {
   UiAudio audio;audio.Load("mouse_click",Pcm({1000,-1000}),.0971999988f);
   Check(audio.Play("mouse_click")>0,"real SDL dummy device admits callback playback");
   const Uint32 start=SDL_GetTicks();while(audio.ActiveVoices() && SDL_GetTicks()-start<2000)SDL_Delay(5);
   Check(audio.ActiveVoices()==0,"real SDL callback consumes submitted PCM");
   bool rejected=false;std::thread other([&]{rejected=Reject([&]{audio.Play("mouse_click");});});other.join();Check(rejected,"cross-thread playback rejects outside owner contract");
   audio.Play("mouse_click"); // Destruction must close/wait even with pending voice.
  }
  Check(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO),"repeated device destroy returns acquired SDL audio subsystem reference");
  for(int arg=1;arg<argc;++arg){
   std::ifstream file(argv[arg],std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open authorized PCM fixture");
   auto length=file.tellg();if(length<=0 || length>44100*4)throw std::runtime_error("Authorized PCM fixture outside bound");
   std::vector<uint8_t> data(static_cast<size_t>(length));file.seekg(0);if(!file.read(reinterpret_cast<char*>(data.data()),data.size()))throw std::runtime_error("Cannot read authorized PCM fixture");
   UiAudio audio;const char *alias=arg==1?"mouse_click":"mouse_over";const float volume=arg==1?.0971999988f:.170099989f;audio.Load(alias,data,volume);Check(audio.Play(alias)>0,"authorized PCM admitted to real dummy callback (not audibility)");
   const auto start=SDL_GetTicks();while(audio.ActiveVoices() && SDL_GetTicks()-start<3000)SDL_Delay(5);Check(audio.ActiveVoices()==0,"authorized PCM completes through real SDL callback");
  }
 }
 }catch(const std::exception &e){std::fprintf(stderr,"audio test: %s\n",e.what());++failures;}
 std::printf("UI audio: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
