#include <linux/client/window/ui_audio_assets.h>
#include <linux/client/window/ui_audio.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <fstream>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
void Word(std::vector<uint8_t>&bytes,size_t index,uint32_t value){for(unsigned i=0;i<4;++i)bytes[8+index*4+i]=value>>(8*i);}
std::vector<uint8_t> Valid(){std::vector<uint8_t> bytes(56,0);std::memcpy(bytes.data(),"KUASND01",8);const std::array<uint32_t,11> words={2648,1,44100,16,1,2,0x3f800000,0x3e800000,2,4,0};for(size_t i=0;i<words.size();++i)Word(bytes,i,words[i]);bytes[52]=0xff;bytes[53]=0x7f;bytes[55]=0x80;return bytes;}
}
int main(int argc,char **argv){try{
 auto bytes=Valid();auto sound=DecodeUiSound(bytes);bytes[52]=0;
 Check(sound.volume==.25f && sound.pcm==std::vector<uint8_t>({255,127,0,128}),"metadata decoder preserves exact owned PCM and exported alias volume");
 for(size_t field=0;field<11;++field){auto bad=Valid();Word(bad,field,field==10?1:0xffffffff);Check(Reject([&]{DecodeUiSound(bad);}),"each unsupported metadata field rejects");}
 for(size_t size:{0u,7u,51u,52u,55u,57u,176453u}){auto bad=Valid();bad.resize(size);Check(Reject([&]{DecodeUiSound(bad);}),"truncation trailing bytes and bounds reject");}
 auto bad=Valid();bad[0]='X';Check(Reject([&]{DecodeUiSound(bad);}),"wrong resource signature rejects");
 for(auto value:{0x7f800000u,0x7fc00000u,0xbf800000u,0x40000000u}){bad=Valid();Word(bad,7,value);Check(Reject([&]{DecodeUiSound(bad);}),"nonfinite or out-of-range volume rejects");}
 for(auto samples:{0u,88201u}){bad=Valid();Word(bad,8,samples);Check(Reject([&]{DecodeUiSound(bad);}),"sample count resource limits reject");}
 auto maximum=Valid();maximum.resize(176452);Word(maximum,8,88200);Word(maximum,9,176400);
 Check(DecodeUiSound(maximum).pcm.size()==176400,"exact two-second admitted resource bound accepted");
 {
  namespace fs=std::filesystem;
  const auto path=fs::temp_directory_path()/("kisak-ui-sound-"+std::to_string(SDL_GetPerformanceCounter()));
  if(!fs::create_directory(path))throw std::runtime_error("Cannot create isolated fixture directory");
  struct Cleanup { fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);} } cleanup{path};
  auto write=[&](const char *name,const std::vector<uint8_t>&data){std::ofstream file(path/name,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),data.size());if(!file)throw std::runtime_error("Cannot write fixture resource");};
  write("mouse_click.sound",Valid());auto invalid=Valid();invalid[0]='X';write("mouse_over.sound",invalid);
  SDL_setenv("SDL_AUDIODRIVER","dummy",1);UiAudio audio;
  Check(Reject([&]{LoadMenuAudio(audio,path.string());}),"invalid second alias rejects complete bundle");
  Check(Reject([&]{audio.Play("mouse_click");}) && Reject([&]{audio.Play("mouse_over");}),"metadata validation failure publishes neither sound to provider");
  write("mouse_over.sound",Valid());LoadMenuAudio(audio,path.string());
  Check(audio.Play("mouse_click")>0 && audio.Play("mouse_over")>0,"valid retry loads both aliases after prior validation failure (dummy device)");
 }
 if(argc==2){SDL_setenv("SDL_AUDIODRIVER","dummy",1);UiAudio audio;LoadMenuAudio(audio,argv[1]);Check(audio.Play("mouse_click")>0 && audio.Play("mouse_over")>0,"actual prepared bundle loads both aliases into real SDL dummy callback (not audibility)");}
 }catch(const std::exception&e){std::fprintf(stderr,"audio assets: %s\n",e.what());++failures;}
 std::printf("UI audio assets: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
