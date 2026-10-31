#include <linux/client/window/cpu_renderer.h>
#include <gfx_d3d/rb_backend.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name) {++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
template<class F> bool Reject(F f) {try {f();return false;}catch(const std::exception&){return true;}}
bool Pixel(CpuRenderer &r,int x,int y,std::array<uint8_t,4> color) {
 auto p=r.Pixels();return std::equal(color.begin(),color.end(),p.bytes.begin()+y*p.pitch+x*4);
}
GfxCmdHeader *Append(void *p,GfxRenderCommand id,int bytes) {return R_AppendCommand(*static_cast<GfxCmdArray*>(p),16384,id,bytes);}
}
int main() {
 try {
  CpuRenderer r;r.Initialize(4,3);
  std::array<uint8_t,32> source={255,0,0,0, 0,255,0,17, 0,0,255,128, 99,99,99,99,
                               255,255,0,0, 255,0,255,17, 0,255,255,128, 99,99,99,99};
  const auto image=r.Upload({3,2,16,source});source.fill(13);
  Check(r.TextureCount()==1,"upload owns padded source pixels independently");
  const float black[4]={0,0,0,1},white[4]={1,1,1,1};
  r.BeginFrame();r.Clear(black);r.Blit(image,-1,1,3,2);
  Check(Reject([&]{r.Release(image);}) && Reject([&]{r.Upload({1,1,4,source});}),"recorded bindings prohibit resource mutation and release");
  r.Submit();
  Check(Pixel(r,0,1,{0,255,0,255}) && Pixel(r,1,1,{0,0,255,255}) && Pixel(r,0,2,{255,0,255,255}) && Pixel(r,1,2,{0,255,255,255}) && Pixel(r,2,1,{0,0,0,255}),"clipping preserves source mapping and ignores source alpha like X8 surface");
  Check(Reject([&]{r.Release(image);}),"pending presentation retains immutable image resources");
  r.Resize(4,3);r.BeginFrame();r.Blit(image,0,0,3,2);r.Clear(white);r.Submit();
  Check(Pixel(r,0,0,{255,255,255,255}) && Pixel(r,2,1,{255,255,255,255}),"clear after blit preserves actual command order");
  r.Resize(4,3);r.BeginFrame();
  Check(Reject([&]{r.Blit(image,0,0,6,4);}),"unsupported scaling rejects explicitly");
  r.Blit(image,INT_MAX,INT_MAX,3,2);r.Blit(image,INT_MIN,INT_MIN,3,2);r.Clear(black);r.Submit();
  Check(Pixel(r,0,0,{0,0,0,255}) && Pixel(r,3,2,{0,0,0,255}),"extreme offscreen destinations clip without signed overflow");
  r.Resize(4,3);
  alignas(4) std::array<uint8_t,16384> commands{};GfxCmdArray list{commands.data(),0,0,nullptr};
  R_EmitClearScreenCommand(&list,Append,1,white,1,0);
  R_EmitStretchRawCommand(&list,Append,0,0,3,2,3,2,reinterpret_cast<const uint8_t*>(1));
  R_EmitEndCommand(&list,Append);
  const std::vector<uint8_t> before(r.Pixels().bytes.begin(),r.Pixels().bytes.end());
  Check(Reject([&]{r.SubmitCommands({commands.data(),size_t(list.usedTotal)});}) &&
        std::equal(before.begin(),before.end(),r.Pixels().bytes.begin()) && r.Status()==CpuRenderer::State::ready,
        "external raw pointer rejected before prior clear or any dereference");
  GfxCmdStretchRaw raw;std::memcpy(&raw,commands.data()+28,sizeof(raw));
  Check(raw.header.id==RC_STRETCH_RAW && raw.header.byteCount==40 && raw.cols==3 && raw.rows==2 && raw.client==0 && raw.dirty==1,"new producer uses existing engine raw command ABI");
  r.Release(image);r.BeginFrame();Check(Reject([&]{r.Blit(image,0,0,3,2);}),"released handle cannot generate new binding");
  r.Shutdown();r.Initialize(4,3);const auto fresh=r.Upload({1,1,4,source});
  Check(fresh!=image,"handle generations survive renderer restart");
  r.BeginFrame();Check(Reject([&]{r.Blit(image,0,0,3,2);}),"old restart handle remains invalid even with live resources");
  r.Resize(4,3);
  Check(Reject([&]{r.SubmitCommands({commands.data(),size_t(list.usedTotal)});}),"old external raw list stays rejected after allocation reuse/restart");
  for(int i=1;i<64;++i)r.Upload({1,1,4,source});
  Check(r.TextureCount()==64 && Reject([&]{r.Upload({1,1,4,source});}) && r.TextureCount()==64,"resource count budget rejects transactionally");
  r.BeginFrame();int accepted=0;bool exhausted=false;
  for(int i=0;i<256;++i) {try {r.Blit(fresh,0,0,1,1);++accepted;}catch(const std::length_error&){exhausted=true;break;}}
  r.Submit();Check(exhausted && accepted>0 && r.Status()==CpuRenderer::State::pending,"raw command exhaustion retains valid prior commands and reserved end capacity");
  r.Shutdown();Check(r.TextureCount()==0 && r.Pixels().bytes.empty(),"shutdown releases all owned image resources and bindings");
 } catch(const std::exception &error) {std::fprintf(stderr,"raw test failed: %s\n",error.what());++failures;}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
