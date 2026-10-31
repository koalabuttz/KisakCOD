#include <linux/client/window/cpu_renderer.h>
#include <client/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name) {++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
template<class F> bool Reject(F f) {try{f();return false;}catch(const std::exception&){return true;}}
bool Pixel(CpuRenderer&r,int x,int y,std::array<uint8_t,4> expected) {auto p=r.Pixels();return std::equal(expected.begin(),expected.end(),p.bytes.begin()+y*p.pitch+x*4);}
GfxCmdHeader* Append(void *p,GfxRenderCommand id,int size) {return R_AppendCommand(*static_cast<GfxCmdArray*>(p),16384,id,size);}
}
int main() {
 try {
  CpuRenderer r;r.Initialize(8,8);
  std::array<uint8_t,16> pixels={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  const auto texture=r.Upload({2,2,8,pixels});const auto material=r.RegisterUiMaterial(texture);
  ScreenPlacement place{};const float black[4]={0,0,0,1},white[4]={1,1,1,1},half[4]={1,1,1,.5f};
  r.BeginFrame();r.Clear(black);r.DrawUiImage(material,place,0,0,4,4,5,5,half);r.Submit();
  Check(Pixel(r,0,0,{128,0,0,255}) && Pixel(r,3,0,{0,128,0,255}) && Pixel(r,0,3,{0,0,128,255}),"actual UI/CL/placement caller scales image and applies packed alpha");
  Check(Pixel(r,1,0,{96,32,0,255}) && Pixel(r,1,1,{80,32,32,255}),"bilinear texel-center interpolation has independently calculated colors");
  Check(Pixel(r,4,0,{0,0,0,255}) && Pixel(r,0,4,{0,0,0,255}),"half-open UI rectangle leaves adjacent pixels unchanged");
  r.Resize(8,8);r.BeginFrame();r.Clear(black);r.DrawUiImage(material,place,0,0,-4,4,5,5,white);r.Submit();
  Check(Pixel(r,0,0,{0,255,0,255}) && Pixel(r,3,0,{255,0,0,255}),"real UI negative width flips UV coordinates");
  place.scaleVirtualToReal[0]=place.scaleVirtualToReal[1]=2;place.realViewableMin[0]=place.realViewableMin[1]=1;
  r.Resize(8,8);r.BeginFrame();r.Clear(black);r.DrawUiImage(material,place,0,0,2,2,1,1,white);r.Submit();
  Check(Pixel(r,1,1,{255,0,0,255}) && Pixel(r,4,1,{0,255,0,255}) && Pixel(r,0,0,{0,0,0,255}),"actual screen placement scales and offsets virtual UI rectangle");
  const float tint[4]={.5f,1,1,.5f};
  r.Resize(8,8);r.BeginFrame();r.Clear(black);r.DrawUiImage(material,place,0,0,4,4,5,5,tint);r.Submit();
  Check(Pixel(r,0,0,{64,0,0,255}),"actual BGRA color packing maps tint and alpha to correct channels");
  r.Resize(8,8);r.BeginFrame();r.Clear(black);r.DrawUiImage(material,place,-2,-2,4,4,5,5,white);r.Submit();
  Check(Pixel(r,0,0,{159,191,191,255}) && Pixel(r,1,1,{255,255,255,255}) && Pixel(r,2,0,{0,0,0,255}),"clipping retains original UI texture interpolation coordinates");
  r.Resize(8,8);r.BeginFrame();
  Check(Reject([&]{r.DrawUiImage(material,place,INFINITY,0,4,4,5,5,white);}) && Reject([&]{r.DrawUiImage(material,place,0,0,1e30f,4,5,5,white);}),"nonfinite and huge geometry rejected before conversion");
  float invalidTint[4]={NAN,1,1,1};
  Check(Reject([&]{r.DrawUiImage(material,place,0,0,4,4,5,5,invalidTint);}),"nonfinite tint rejected before engine byte packing");
  r.DrawUiImage(material,place,0,0,4,4,5,5,white);r.Submit();
  Check(Pixel(r,0,0,{255,0,0,255}),"failed actual UI call restores scope for subsequent valid draw");
  Check(Reject([&]{R_AddCmdDrawStretchPic(0,0,1,1,0,0,1,1,white,reinterpret_cast<Material*>(1));}),"CPU renderer entry rejects unscoped arbitrary material pointer without dereference");
  r.Resize(8,8);alignas(4) std::array<uint8_t,16384> commands{};GfxCmdArray list{commands.data(),0,0,nullptr};
  R_EmitClearScreenCommand(&list,Append,1,black,1,0);
  R_EmitStretchPicCommand(&list,Append,0,0,4,4,0,0,1,1,white,reinterpret_cast<Material*>(1));R_EmitEndCommand(&list,Append);
  const std::vector<uint8_t> before(r.Pixels().bytes.begin(),r.Pixels().bytes.end());
  Check(Reject([&]{r.SubmitCommands({commands.data(),size_t(list.usedTotal)});}) && std::equal(before.begin(),before.end(),r.Pixels().bytes.begin()) && r.Status()==CpuRenderer::State::ready,"external material command rejects before preceding clear changes pixels");
  const float background[4]={64/255.0f,128/255.0f,191/255.0f,1};
  std::array<uint8_t,4> transparent={255,0,0,0};
  const auto transparentMaterial=r.RegisterUiMaterial(r.Upload({1,1,4,transparent}));
  r.BeginFrame();r.Clear(background);r.DrawUiImage(transparentMaterial,place,0,0,2,2,5,5,half);r.Submit();
  Check(Pixel(r,0,0,{64,128,191,255}),"zero source alpha preserves nonblack destination despite tint alpha");
  r.Resize(8,8);transparent[3]=128;
  const auto partialMaterial=r.RegisterUiMaterial(r.Upload({1,1,4,transparent}));
  r.BeginFrame();r.Clear(background);r.DrawUiImage(partialMaterial,place,0,0,2,2,5,5,half);r.Submit();
  Check(Pixel(r,0,0,{112,96,143,255}),"source alpha128 times tint alpha128 blends independently calculated nonblack destination");
 } catch(const std::exception&e) {std::fprintf(stderr,"UI test failed: %s\n",e.what());++failures;}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
