#include <linux/client/window/cpu_renderer.h>
#include <linux/client/window/window_host.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace kisak::window;
namespace {
int checks,failures;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
GfxCmdHeader *Allocate(void *p,GfxRenderCommand id,int bytes){return R_AppendCommand(*static_cast<GfxCmdArray*>(p),128,id,bytes);}
struct Sink:EventSink {uint32_t NowMilliseconds() const override{return SDL_GetTicks();}void Key(uint32_t,int,bool)override{}void Text(uint32_t,std::string_view)override{}void Mouse(uint32_t,int,int,int,int)override{}void Resize(int,int)override{}void Focus(bool)override{}void Quit()override{}};
}
int main(){try{
 const float red[4]={1,0,0,1},blue[4]={0,0,1,1};
 alignas(4) std::array<uint8_t,128> bytes{};GfxCmdArray list{bytes.data(),0,0,nullptr};R_EmitClearScreenCommand(&list,Allocate,1,red,1,0);R_EmitEndCommand(&list,Allocate);
 CpuRenderer r;r.Initialize(8,8);r.BeginFrame();r.Clear(blue);
 Check(Reject([&]{r.SubmitCommands({bytes.data(),size_t(list.usedTotal)});})&&r.Status()==CpuRenderer::State::recording,"external submission cannot replace an owned recording");r.CancelFrame();
 auto invalid=[&](CpuRenderer &v){v.BeginFrame();v.Clear(red);v.SetViewport(1,1,2,2);v.Clear(blue);};
 invalid(r);for(int n=0;n<7;++n)if(!Reject([&]{r.Submit();}))throw std::runtime_error("expected preflight failure");
 CpuRenderer reference;reference.Initialize(8,8);invalid(reference);
 auto remaining=[&](CpuRenderer &v){int count=0;while(!Reject([&]{v.Clear(red);}))++count;return count;};
 const int afterFailure=remaining(r),withoutFailure=remaining(reference);
 Check(afterFailure==withoutFailure&&r.Status()==CpuRenderer::State::recording,"rejected Submit restores sealing metadata and consumes no recording capacity");r.CancelFrame();reference.CancelFrame();
 {
 Sink sink;Config config;config.width=8;config.height=8;config.hidden=true;WindowHost window(sink,config);
 std::array<uint8_t,4> texel{255,255,255,255};auto texture=r.Upload({1,1,4,texel});
 {auto frame=r.BeginScopedFrame(window);r.Clear(red);}
 Check(r.Status()==CpuRenderer::State::ready&&r.Presented()==0&&r.TextureCount()==1,"abandoned recording releases frame but retains owned assets");
 try{auto frame=r.BeginScopedFrame(window);r.Clear(red);throw std::runtime_error("recording error");}catch(const std::runtime_error&){}
 Check(r.Status()==CpuRenderer::State::ready&&r.TextureCount()==1,"recording exception cancels through scoped destruction");
 {auto frame=r.BeginScopedFrame(window);r.Clear(red);
  Check(Reject([&]{auto nested=r.BeginScopedFrame(window);})&&r.Status()==CpuRenderer::State::recording,"nested scope rejects without replacing active work");
  Check(Reject([&]{r.Release(texture);}),"recorded-frame resources remain immutable until ready");
  frame.Finish();Check(r.Status()==CpuRenderer::State::ready&&r.Presented()==1,"scoped finish uses actual submission and SDL presentation");
  Check(Reject([&]{frame.Finish();})&&r.Presented()==1,"finished scope cannot present twice");
  frame.Cancel();frame.Cancel();Check(r.Status()==CpuRenderer::State::ready&&r.Presented()==1,"finished scope cancellation is idempotent");}
 const auto shown=r.Presented();
 {auto frame=r.BeginScopedFrame(window);r.Clear(blue);r.SetViewport(1,1,2,2);r.Clear(red);
  bool original=false;try{frame.Finish();}catch(const std::invalid_argument &e){original=std::string_view(e.what())=="Clear after subviewport is unsupported";}
  Check(original&&r.Status()==CpuRenderer::State::ready&&r.Presented()==shown&&r.Pixels().bytes[0]==255&&r.TextureCount()==1,"failed scoped submit cancels without changing preflight pixels count or resources");}
 {auto frame=r.BeginScopedFrame(window);r.Clear(blue);bool rejected=false;std::thread worker([&]{rejected=Reject([&]{frame.Finish();});});worker.join();
  Check(rejected&&r.Status()==CpuRenderer::State::recording,"wrong-thread finish cannot cancel owner-thread recording");frame.Finish();}
 {auto stale=r.BeginScopedFrame(window);r.Clear(red);r.CancelFrame();r.BeginFrame();r.Clear(blue);
  Check(Reject([&]{stale.Finish();})&&r.Status()==CpuRenderer::State::recording,"stale finish cannot submit replacement generation");}
 Check(r.Status()==CpuRenderer::State::recording,"stale scope destructor cannot cancel replacement generation");r.Submit();r.Present(window);
 {auto stale=r.BeginScopedFrame(window);r.Clear(red);r.Submit();r.Resize(8,8);
  Check(Reject([&]{stale.Finish();})&&r.Status()==CpuRenderer::State::ready&&r.TextureCount()==1,"successful resize cancels pending generation while retaining assets");}
 {auto frame=r.BeginScopedFrame(window);r.Clear(blue);
  Check(Reject([&]{r.Resize(0,8);})&&r.Status()==CpuRenderer::State::recording,"failed resize retains active frame ownership");frame.Finish();}
 {auto stale=r.BeginScopedFrame(window);r.Clear(red);r.Shutdown();r.Initialize(8,8);r.BeginFrame();r.Clear(blue);
  Check(Reject([&]{stale.Finish();}),"restart invalidates old frame scope");}
 Check(r.Status()==CpuRenderer::State::recording,"pre-restart scope cannot cancel new renderer generation");r.Submit();r.Present(window);
 Check(Reject([&]{r.Release(texture);})&&r.Status()==CpuRenderer::State::ready,"old resource handle rejects in ready state after restart");
 {auto cancelled=r.BeginScopedFrame(window);r.Shutdown();Check(!Reject([&]{cancelled.Cancel();cancelled.Cancel();})&&r.Status()==CpuRenderer::State::stopped,"shutdown-invalidated scope cancellation is idempotent");}r.Initialize(8,8);
 texture=r.Upload({1,1,4,texel});const auto beforeQuit=r.Presented();
 {auto frame=r.BeginScopedFrame(window);r.Clear(red);window.RequestQuit();
  Check(Reject([&]{frame.Finish();})&&r.Status()==CpuRenderer::State::ready&&r.Presented()==beforeQuit&&r.Pixels().bytes[0]==255,"actual closed-window presentation failure cancels pending work without pixel rollback or count increment");}
 r.Release(texture);Check(r.TextureCount()==0,"failed presentation releases frame ownership so resources can be removed");
 }
 {Sink sink;Config config;config.width=8;config.height=8;config.hidden=true;WindowHost window(sink,config);auto frame=r.BeginScopedFrame(window);r.Clear(blue);frame.Finish();Check(r.Status()==CpuRenderer::State::ready&&r.Pixels().bytes[2]==255,"same renderer presents after native window recreation");}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL exception: %s\n",e.what());return 1;}}
