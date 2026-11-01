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
  r.Resize(8,8);
  std::vector<PixelBuffer> levels(4);const std::array<std::array<uint8_t,4>,4> colors={{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}}};
  for(unsigned i=0;i<4;++i){levels[i].Resize(8>>i,8>>i);auto b=levels[i].Bytes();for(size_t at=0;at<b.size();at+=4)std::copy(colors[i].begin(),colors[i].end(),b.begin()+at);}
  const auto mipTexture=r.UploadMipChain(levels);const auto mipMaterial=r.RegisterUiMaterial(mipTexture,true);
  for(auto &level:levels)std::fill(level.Bytes().begin(),level.Bytes().end(),0);
  for(int size:{8,6,4,3,2,1}){
   r.Resize(8,8);r.BeginFrame();r.Clear(black);r.DrawUiImage(mipMaterial,place,0,0,float(size),float(size),5,5,white);r.Submit();
   const int expected=size>=6?0:size>=3?1:size==2?2:3;
   Check(Pixel(r,0,0,colors[expected]),"owned genuine mip selection follows nearest level at isotropic footprint");
  }
  r.Resize(8,8);r.BeginFrame();r.Clear(black);r.DrawUiImage(mipMaterial,place,0,0,4,2,5,5,white);
  r.Submit();
  Check(Pixel(r,0,0,colors[1]),"nonuniform footprint selects the supported minor-axis mip without dropping material");
  r.Resize(8,8);
  std::vector<PixelBuffer> pattern(4);
  const std::array<std::array<uint8_t,4>,4> rows={{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}}};
  for(unsigned level=0;level<4;++level) {
   pattern[level].Resize(8>>level,8>>level);auto bytes=pattern[level].Bytes();const int size=8>>level;
   for(int y=0;y<size;++y)for(int x=0;x<size;++x){
    const std::array<uint8_t,4> color=level==1?rows[y]:level==2?(y==0?std::array<uint8_t,4>{255,255,0,255}:std::array<uint8_t,4>{0,255,255,255}):std::array<uint8_t,4>{255,0,255,255};
    std::copy(color.begin(),color.end(),bytes.begin()+(y*size+x)*4);
   }
  }
  const auto patterned=r.RegisterUiMaterial(r.UploadMipChain(pattern),true);
  auto drawPattern=[&](float x,float y,float w,float h){r.Resize(8,8);r.BeginFrame();r.Clear(black);r.DrawUiImage(patterned,place,x,y,w,h,5,5,white);r.Submit();};
  drawPattern(0,0,4,2);
  Check(Pixel(r,0,0,{128,128,0,255}) && Pixel(r,0,1,{128,128,255,255}),"two vertical taps average independently computed colored mip rows");
  drawPattern(0,0,4,-2);
  Check(Pixel(r,0,0,{128,128,255,255}) && Pixel(r,0,1,{128,128,0,255}),"flipped UV preserves symmetric anisotropic taps");
  drawPattern(0,-1,4,2);
  Check(Pixel(r,0,0,{128,128,255,255}) && Pixel(r,0,1,{0,0,0,255}),"clipping preserves original anisotropic footprint and UVs");
  drawPattern(0,.25f,4,2);
  Check(Pixel(r,0,0,{191,64,0,255}),"each displaced tap clamps independently at texture edge");
  drawPattern(0,0,8,1);
  Check(Pixel(r,0,0,{128,255,128,255}),"ratio above two coarsens the minor footprint and still uses exactly two taps");
  drawPattern(0,0,8/1.40f,4);
  Check(Pixel(r,0,0,{255,0,255,255}),"minor footprint below nearest-mip boundary selects stored base level");
  drawPattern(0,0,8/1.43f,4);
  Check(Pixel(r,0,0,{237,18,0,255}),"minor footprint above nearest-mip boundary selects and filters stored mip one");
  drawPattern(0,0,4,3.999999f);
  Check(Pixel(r,0,0,{255,0,0,255}),"existing isotropic tolerance retains prior single-tap pixel result");
  drawPattern(0,0,4,8/2.01f);
  Check(Pixel(r,0,0,{254,1,0,255}),"near-isotropic two-tap footprint approaches the original bilinear result");
  r.Resize(8,8);
  for(int y=0;y<4;++y)for(int x=0;x<4;++x)std::copy(rows[x].begin(),rows[x].end(),pattern[1].Bytes().begin()+(y*4+x)*4);
  const auto horizontal=r.RegisterUiMaterial(r.UploadMipChain(pattern),true);
  r.BeginFrame();r.Clear(black);r.DrawUiImage(horizontal,place,0,0,2,4,5,5,white);r.Submit();
  Check(Pixel(r,0,0,{128,128,0,255}) && Pixel(r,1,0,{128,128,255,255}),"transposed major axis produces transposed two-tap colors");
  r.Resize(8,8);
  for(int y=0;y<4;++y)for(int x=0;x<4;++x){auto bytes=pattern[1].Bytes();std::copy(rows[y].begin(),rows[y].end(),bytes.begin()+(y*4+x)*4);bytes[(y*4+x)*4+3]=y==0?64:192;}
  const auto translucent=r.RegisterUiMaterial(r.UploadMipChain(pattern),true);
  r.BeginFrame();r.Clear(background);r.DrawUiImage(translucent,place,0,0,4,2,5,5,half);r.Submit();
  Check(Pixel(r,0,0,{80,128,143,255}),"RGBA taps average before packed tint and one source-alpha blend");
  r.Resize(8,8);const auto incomplete=r.RegisterUiMaterial(r.Upload(pattern.front().View()),true);
  r.BeginFrame();r.Clear(black);r.DrawUiImage(incomplete,place,0,0,4,2,5,5,white);
  const auto saved=std::vector<uint8_t>(r.Pixels().bytes.begin(),r.Pixels().bytes.end());
  Check(Reject([&]{r.Submit();})&&std::equal(saved.begin(),saved.end(),r.Pixels().bytes.begin()),"nonuniform minification with incomplete mip chain still rejects before clear mutation");
  r.Resize(8,8);
  Check(Reject([&]{r.UploadMipChain(std::span<const PixelBuffer>(levels).first(3));}),"incomplete chain upload rejected");
  levels[1].Resize(3,4);Check(Reject([&]{r.UploadMipChain(levels);}),"invalid chain shape rejected");
  Check(Reject([&]{r.Release(mipTexture);}),"material pins entire owned mip resource");
  {
   CpuRenderer budget;budget.Initialize(1,1);std::vector<PixelBuffer> tiny(2);tiny[0].Resize(2,2);tiny[1].Resize(1,1);
   std::vector<CpuRenderer::TextureHandle> handles;for(int i=0;i<64;++i)handles.push_back(budget.UploadMipChain(tiny));
   Check(Reject([&]{budget.UploadMipChain(tiny);}) && budget.TextureCount()==64,"mip uploads share bounded image resource budget transactionally");
   budget.Release(handles.back());const auto replacement=budget.UploadMipChain(tiny);
   Check(replacement!=handles.back() && budget.TextureCount()==64,"release refunds resource capacity without reusing stale handle");
   budget.Shutdown();Check(budget.TextureCount()==0,"shutdown releases every mip resource");
  }
  r.ReleaseUiMaterial(mipMaterial);r.Release(mipTexture);
  Check(Reject([&]{r.RegisterUiMaterial(mipTexture,true);}),"released mip chain handle cannot be rebound");
 } catch(const std::exception&e) {std::fprintf(stderr,"UI test failed: %s\n",e.what());++failures;}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
