#include <universal/q_shared.h>
#include <linux/client/window/font_assets.h>
#include <linux/client/window/iwi_assets.h>
#include <linux/client/window/cpu_renderer.h>
#include <client/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <limits>
using namespace kisak::window;
namespace {int checks=0,failures=0;void Check(bool ok,const char*name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);} }
int main(int argc,char **argv){try{
 std::array<Glyph,96> glyphs{};for(unsigned i=0;i<glyphs.size();++i){glyphs[i].letter=i+32;glyphs[i].dx=3;glyphs[i].s1=glyphs[i].t1=1;}
 const char name[]="synthetic/font",material[]="synthetic/material";
 const uint32_t start=16+sizeof(glyphs);std::array<uint32_t,4> header={start,48,96,start+sizeof(name)};
 std::vector<uint8_t> file(start+sizeof(name)+sizeof(material));std::memcpy(file.data(),header.data(),16);std::memcpy(file.data()+16,glyphs.data(),sizeof(glyphs));std::memcpy(file.data()+start,name,sizeof(name));std::memcpy(file.data()+header[3],material,sizeof(material));
 const auto parsed=DecodeFont(file);Check(parsed.name==name&&parsed.materialName==material&&parsed.pixelHeight==48&&parsed.glyphs.size()==96,"actual loose-font offsets and metadata parsed into owned storage");
 auto reject=[&](std::vector<uint8_t> bad,const char*label){bool rejected=false;try{DecodeFont(bad);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,label);};
 auto bad=file;bad.resize(15);reject(bad,"short font header rejected");
 bad=file;bad.resize(start-1);reject(bad,"truncated glyph array rejected before names");
 bad=file;uint32_t offset=16;std::memcpy(bad.data(),&offset,4);reject(bad,"name offset into glyph storage rejected");
 bad=file;bad.back()='x';reject(bad,"unterminated material name rejected");
 bad=file;uint32_t count=0xffffffff;std::memcpy(bad.data()+8,&count,4);reject(bad,"oversized glyph count rejected before allocation");
 bad=file;bad[16]=33;reject(bad,"wrong ASCII glyph ordering rejected");
 bad=file;float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(bad.data()+16+offsetof(Glyph,s0),&nan,4);reject(bad,"nonfinite atlas coordinate rejected");
 bad=file;std::memset(bad.data()+16,0,sizeof(Glyph));Check(parsed.glyphs[0].letter==32&&parsed.name==name,"parsed metadata and glyphs do not retain caller file memory");
 Check(parsed.glyphs[0].pixelWidth==0&&parsed.glyphs[0].dx==3,"zero-area glyph metrics preserve advance");
 int first=1;PixelBuffer atlas;bool reload=false;
 if(argc>3 && std::string_view(argv[1])=="--atlas"){atlas=LoadIwiFile(argv[2]);first=3;reload=true;}
 CpuRenderer renderer;CpuRenderer::FontHandle previous=0;
 for(int i=first;i<argc;++i){
  const auto real=LoadFontFile(argv[i]);Check(real.glyphs.size()>=96&&!real.name.empty(),"genuine exported font parsed without metric substitution");
  std::printf("font=%s material=%s glyphs=%zu pixel_height=%d\n",real.name.c_str(),real.materialName.c_str(),real.glyphs.size(),real.pixelHeight);
  if(reload)for(int cycle=0;cycle<2;++cycle){
   renderer.Initialize(128,64);auto texture=renderer.Upload(atlas.View());auto material=renderer.RegisterUiMaterial(texture);
   const auto font=renderer.RegisterFont(material,real.name,real.pixelHeight,real.glyphs);
   ScreenPlacement placement{};const float color[4]={1,1,1,1};
   renderer.BeginFrame();renderer.DrawUiText(font,placement,"A",1,24,48,5,5,.5f,color);renderer.Submit();
   bool visible=false;auto pixels=renderer.Pixels().bytes;for(size_t byte=0;byte<pixels.size();++byte)if(byte%4!=3&&pixels[byte])visible=true;
   renderer.Shutdown();Check(visible&&font!=previous&&renderer.FontCount()==0&&renderer.TextureCount()==0&&renderer.Pixels().bytes.empty(),"genuine font reload renders through actual UI and pending teardown releases all resources");previous=font;
  }
 }
}catch(const std::exception&e){std::fprintf(stderr,"font parser test: %s\n",e.what());++failures;}
std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;}
