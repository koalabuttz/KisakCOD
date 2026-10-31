#include <linux/client/window/menu_assets.h>
#include <client/client.h>
#include <array>
#include <bit>
#include <cstdio>
#include <vector>
#include <limits>
using namespace kisak::window;
namespace {int checks=0,failures=0;void Check(bool ok,const char*n){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",n);}template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}}
int main(int argc,char**argv){try{
 std::vector<uint8_t> bytes{'K','U','M','E','N','U','0','1'};
 auto word=[&](uint32_t value){for(int i=0;i<4;++i)bytes.push_back(value>>(8*i));};
 for(float f:{-74.f,54.f,300.f,80.f,1.f,0.f,0.f,.375f,.69f,.69f,.69f,1.f,.25f,.4f,.55f})word(std::bit_cast<uint32_t>(f));
 for(uint32_t v:{2u,1u,0u,1u,3u,0u,0u,0x800000u,4u,3u,9u})word(v);
 for(char c:std::string("A Afixture:1"))bytes.push_back(c);
 auto item=DecodeMenuText(bytes);Check(item.text=="A A"&&item.source=="fixture:1"&&item.item.window.rect.x==-74&&item.item.textStyle==3,"actual item scalar format preserves text geometry style and source");
 auto invalid=bytes;invalid.resize(111);Check(Reject([&]{DecodeMenuText(invalid);}),"truncated menu header rejected");
 invalid=bytes;invalid.push_back(0);Check(Reject([&]{DecodeMenuText(invalid);}),"trailing menu payload rejected");
 invalid=bytes;invalid[0]=0;Check(Reject([&]{DecodeMenuText(invalid);}),"invalid menu format rejected");
 invalid=bytes;uint32_t nan=0x7fc00000;for(int n=0;n<4;++n)invalid[8+n]=nan>>(8*n);Check(Reject([&]{DecodeMenuText(invalid);}),"nonfinite menu geometry rejected before allocation");
 invalid=bytes;invalid[84]=6;Check(Reject([&]{DecodeMenuText(invalid);}),"unsupported style metadata rejected");
 invalid=bytes;invalid[92]=2;Check(Reject([&]{DecodeMenuText(invalid);}),"cinematic item flags rejected");
 invalid=bytes;invalid[112]='\t';Check(Reject([&]{DecodeMenuText(invalid);}),"unsupported control text rejected");
 invalid=bytes;invalid[112]='^';invalid[113]='1';Check(Reject([&]{DecodeMenuText(invalid);}),"unsupported color escape rejected");
 invalid=bytes;invalid[104]=255;Check(Reject([&]{DecodeMenuText(invalid);}),"invalid string span rejected");
 bytes[112]='B';Check(item.text=="A A","decoded text owns input lifetime");
 if(argc>=3) {
  CpuRenderer renderer;CpuRenderer::FontHandle prior=0;
  for(int cycle=0;cycle<2;++cycle){renderer.Initialize(640,480);auto genuine=LoadMenuBundle(renderer,argv[1],argv[2]);
   ScreenPlacement placement{};ScrPlace_SetupFloatViewportForDisplay(&placement,0,0,640,480,1,640,480);
   renderer.BeginFrame();const float black[4]={0,0,0,1};renderer.Clear(black);renderer.DrawMenuText(genuine.fonts,placement,genuine.item,genuine.text,genuine.item.window.foreColor);renderer.Submit();
   bool visible=false;auto p=renderer.Pixels();for(size_t n=0;n<p.bytes.size();n+=4)visible|=p.bytes[n]!=0;
   auto handle=genuine.fonts.text;renderer.Shutdown();Check(visible&&handle!=prior&&renderer.FontCount()==0&&renderer.TextureCount()==0,"genuine bundle draws and pending teardown/reload releases all owned roles");prior=handle;
  }
  renderer.Initialize(640,480);Check(Reject([&]{LoadMenuBundle(renderer,argv[1],"/missing/menu-images");})&&renderer.FontCount()==0&&renderer.TextureCount()==0,"explicit missing genuine atlas fails without resource fallback/leak");
  if(argc==4){std::array<uint8_t,16> pixels{};auto existing=renderer.Upload({2,2,8,pixels});
   Check(Reject([&]{LoadMenuBundle(renderer,argv[3],argv[2]);})&&renderer.FontCount()==0&&renderer.TextureCount()==1,"late genuine role failure rolls back six completed roles and preserves preexisting resource");renderer.Release(existing);}

 }
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"menu assets: %s\n",e.what());return 2;}}
