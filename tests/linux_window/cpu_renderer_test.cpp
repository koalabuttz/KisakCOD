#include <linux/client/window/cpu_renderer.h>
#include <linux/client/window/window_host.h>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name) { ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name); }
template<class F> bool Reject(F f) { try { f();return false; }catch(const std::exception&) { return true; } }
struct Sink final:EventSink {
 uint32_t NowMilliseconds() const override {return SDL_GetTicks();}
 void Key(uint32_t,int,bool) override {} void Text(uint32_t,std::string_view) override {}
 void Mouse(uint32_t,int,int,int,int) override {} void Resize(int,int) override {}
 void Focus(bool) override {} void Quit() override {}
};
GfxCmdHeader *Allocate(void *owner,GfxRenderCommand id,int bytes) {
 return R_AppendCommand(*static_cast<GfxCmdArray*>(owner),16384,id,bytes);
}
}
int main() {
 try {
  alignas(4) std::array<uint8_t,16384> bytes{};GfxCmdArray list{bytes.data(),0,0,nullptr};
  const float color[4]={.25f,.5f,.75f,1};
  R_EmitClearScreenCommand(&list,Allocate,1,color,1,0);R_EmitEndCommand(&list,Allocate);
  GfxCmdClearScreen clear;std::memcpy(&clear,bytes.data(),sizeof(clear));
  Check(list.usedTotal==32 && list.usedCritical==32 && clear.header.id==RC_CLEAR_SCREEN && clear.header.byteCount==28 && clear.whichToClear==1 && clear.color[2]==.75f,"actual shared producer emits engine clear/end layouts");
  GfxCmdHeader end;std::memcpy(&end,bytes.data()+28,4);
  Check(end.id==RC_END_OF_LIST && end.byteCount==4,"actual shared end producer terminates command list");
  alignas(4) std::array<uint8_t,16384> reserveBytes{};
  GfxCmdArray reserve{reserveBytes.data(),0,0,nullptr};
  Check(R_AppendCommand(reserve,16384,RC_STRETCH_PIC,8192)!=nullptr && R_AppendCommand(reserve,16384,RC_STRETCH_PIC,4)==nullptr && !reserve.lastCmd,"noncritical commands retain original 8192-byte critical reserve");
  Check(R_AppendCommand(reserve,16384,RC_CLEAR_SCREEN,8192)!=nullptr && reserve.usedTotal==16384 && reserve.usedCritical==8192,"critical commands may consume reserved capacity");
  Check(!R_AppendCommand(reserve,16384,RC_END_OF_LIST,4) && reserve.usedTotal==16384,"exhaustion leaves counters unchanged");
  reserve.usedTotal=16380;reserve.usedCritical=16380;
  Check(!R_AppendCommand(reserve,16384,RC_STRETCH_PIC,8) && reserve.usedTotal==16380,"critical usage above reserve cannot grant nonexistent storage");
  reserve.usedTotal=-4;Check(!R_AppendCommand(reserve,16384,RC_END_OF_LIST,4),"invalid explicit list rejected before pointer arithmetic");
  Sink sink;Config config;config.width=7;config.height=5;config.hidden=true;
  for(int cycle=0;cycle<2;++cycle) {
   WindowHost window(sink,config);CpuRenderer renderer;renderer.Initialize(7,5);
   renderer.BeginFrame();renderer.Clear(color);renderer.Submit();
   auto view=renderer.Pixels();
   Check(view.bytes[0]==64 && view.bytes[1]==128 && view.bytes[2]==191 && view.bytes[3]==255,"real engine color packing reaches CPU pixel resource");
   Check(Reject([&]{renderer.BeginFrame();}) && Reject([&]{renderer.SubmitCommands({bytes.data(),32});}),"second pending frame is rejected");
   renderer.Present(window);Check(renderer.Presented()==1 && renderer.Status()==CpuRenderer::State::ready,"actual SDL presentation consumes pending engine frame");
   auto invalid=bytes;
   GfxCmdClearScreen different=clear;different.color[0]=1.0f;different.color[1]=0.0f;
   std::memcpy(invalid.data(),&different,sizeof(different));
   GfxCmdHeader unsupported=end;unsupported.id=RC_SAVE_SCREEN;
   std::memcpy(invalid.data()+28,&unsupported,sizeof(unsupported));
   const std::vector<uint8_t> before(renderer.Pixels().bytes.begin(),renderer.Pixels().bytes.end());
   const auto framesBefore=renderer.Presented();
   Check(Reject([&]{renderer.SubmitCommands({invalid.data(),32});}) &&
         std::equal(before.begin(),before.end(),renderer.Pixels().bytes.begin()) &&
         renderer.Presented()==framesBefore && renderer.Status()==CpuRenderer::State::ready,
         "unsupported tail rejects different earlier clear without changing any pixels/state/count");
   invalid=bytes;GfxCmdClearScreen depth=clear;depth.whichToClear=3;
   std::memcpy(invalid.data(),&depth,sizeof(depth));
   Check(Reject([&]{renderer.SubmitCommands({invalid.data(),32});}),"unsupported depth surface rejected with otherwise valid list");
   Check(Reject([&]{renderer.SubmitCommands({bytes.data(),31});}),"truncated end rejected");
   renderer.BeginFrame();renderer.Clear(color);renderer.Submit();renderer.Resize(7,5);
   Check(renderer.Status()==CpuRenderer::State::ready && Reject([&]{renderer.Present(window);}),"successful same-size resize cancels pending frame");
   renderer.BeginFrame();renderer.Clear(color);renderer.Shutdown();renderer.Shutdown();
   Check(renderer.Status()==CpuRenderer::State::stopped && renderer.Pixels().bytes.empty(),"repeated shutdown discards recording and releases resources");
   renderer.Initialize(7,5);renderer.SubmitCommands({bytes.data(),32});renderer.Present(window);
   Check(renderer.Presented()==1,"same renderer restarts with fresh frame accounting");
   renderer.BeginFrame();renderer.Clear(color);renderer.Submit();window.RequestQuit();
   const std::vector<uint8_t> pendingPixels(renderer.Pixels().bytes.begin(),renderer.Pixels().bytes.end());
   Check(Reject([&]{renderer.Present(window);}) && renderer.Status()==CpuRenderer::State::pending &&
         renderer.Presented()==1 && std::equal(pendingPixels.begin(),pendingPixels.end(),renderer.Pixels().bytes.begin()),
         "failed SDL presentation preserves pending frame pixels and completed count");
   renderer.Shutdown();Check(renderer.Status()==CpuRenderer::State::stopped,"shutdown recovers from failed pending presentation");
  }
 } catch(const std::exception &error) { std::fprintf(stderr,"renderer test failed: %s\n",error.what());++failures; }
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
