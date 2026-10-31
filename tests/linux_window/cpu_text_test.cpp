// Synthetic glyph metrics deliberately isolate engine text placement; real asset
// loading/readback is a separate authorized-assets test, never implied by this fixture.
#include <linux/client/window/cpu_renderer.h>
#include <client/client.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <vector>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
bool Lit(CpuRenderer&r,int x,int y){auto p=r.Pixels();return p.bytes[y*p.pitch+x*4]==255;}
GfxCmdHeader *Append(void*p,GfxRenderCommand id,int size){return R_AppendCommand(*static_cast<GfxCmdArray*>(p),16384,id,size);}
}
int main(){try{
 CpuRenderer r;r.Initialize(80,80);ScreenPlacement placement{};
 std::array<uint8_t,16> white;white.fill(255);
 const auto material=r.RegisterUiMaterial(r.Upload({2,2,8,white}));
 std::array<Glyph,96> glyphs{};
 for(unsigned i=0;i<96;++i){auto&g=glyphs[i];g.letter=i+32;g.x0=1;g.y0=-2;g.dx=3;g.pixelWidth=g.pixelHeight=2;g.s1=g.t1=1;}
 glyphs[0].pixelWidth=glyphs[0].pixelHeight=0;glyphs[0].dx=2;
 const auto font=r.RegisterFont(material,"synthetic-fixture",48,glyphs);glyphs['A'-32].dx=99;
 const float black[4]={0,0,0,1},color[4]={1,0,0,1};
 auto draw=[&](std::string_view text,int maximum=1024,float scale=1.f){r.Resize(80,80);r.BeginFrame();r.Clear(black);r.DrawUiText(font,placement,text,maximum,4,4,5,5,scale,color);r.Submit();};
 draw("AB");Check(Lit(r,4,1)&&Lit(r,5,2)&&Lit(r,7,1)&&!Lit(r,6,1),"actual UI normalized scale and engine half-pixel baseline/offset/advance");
 Check(!Lit(r,4,3)&&!Lit(r,3,1),"glyph rectangle clips at precise half-open boundaries");
 draw("A B");Check(Lit(r,9,1)&&!Lit(r,7,1),"zero-area whitespace advances without drawing using copied metrics");
 draw("A\nB");Check(Lit(r,4,49)&&Lit(r,4,1),"newline uses actual font pixel height and resets horizontal baseline");
 draw("A\rB");Check(Lit(r,4,1)&&!Lit(r,7,1),"carriage return resets horizontal position without line advance");
 draw("AB",1);Check(Lit(r,4,1)&&!Lit(r,7,1),"maxChars limits glyph count");
 draw("\nB",1);Check(Lit(r,4,49),"newline does not consume maxChars glyph budget");
 draw("A",1024,2);Check(Lit(r,5,0)&&Lit(r,8,1)&&!Lit(r,9,1),"normalized scale transforms glyph size and baseline consistently");
 r.Resize(80,80);r.BeginFrame();r.Clear(black);
 Check(Reject([&]{r.DrawUiText(font,placement,"^1red",10,4,4,5,5,1,color);})&&Reject([&]{r.DrawUiText(font,placement,"\t",10,4,4,5,5,1,color);})&&Reject([&]{r.DrawUiText(font,placement,"\xc3\xa9",10,4,4,5,5,1,color);}),"unsupported color codes/control/non-ASCII glyphs reject explicitly");
 Check(Reject([&]{r.DrawUiText(font,placement,"A",10,4,4,5,5,1,color,6);}),"unsupported text style rejects without partial command");
 r.DrawUiText(font,placement,"A",10,4,4,5,5,1,color);r.Submit();Check(Lit(r,4,1),"failed text scope restores for a subsequent real UI draw");
 Check(Reject([&]{R_AddCmdDrawText("A",1,reinterpret_cast<Font_s*>(1),0,0,1,1,0,color,0);}),"unscoped font pointer rejects before any lookup");
 r.Resize(80,80);const auto before=std::vector<uint8_t>(r.Pixels().bytes.begin(),r.Pixels().bytes.end());
 alignas(4)std::array<uint8_t,16384> storage{};GfxCmdArray list{storage.data(),0,0,nullptr};
 R_EmitClearScreenCommand(&list,Append,1,black,1,0);R_EmitBaseTextCommand(&list,Append,"A",1,reinterpret_cast<Font_s*>(1),0,0,1,1,0,color,0,-1,0);R_EmitEndCommand(&list,Append);
 Check(Reject([&]{r.SubmitCommands({storage.data(),size_t(list.usedTotal)});})&&std::equal(before.begin(),before.end(),r.Pixels().bytes.begin()),"external text command rejects atomically before preceding clear");
 auto invalid=glyphs;invalid[0].letter=33;
 Check(Reject([&]{r.RegisterFont(material,"bad",48,invalid);})&&r.FontCount()==1,"invalid engine ASCII ordering rejects font registration transactionally");
 Check(Reject([&]{r.ReleaseUiMaterial(material);}),"font pins its atlas material");
 r.ReleaseFont(font);r.BeginFrame();Check(Reject([&]{r.DrawUiText(font,placement,"A",1,4,4,5,5,1,color);}),"released font handle cannot generate commands");
 r.Shutdown();r.Initialize(80,80);auto newMaterial=r.RegisterUiMaterial(r.Upload({2,2,8,white}));auto newFont=r.RegisterFont(newMaterial,"synthetic-new",48,glyphs);
 Check(newFont!=font && r.FontCount()==1,"font handles are not reused across shutdown/restart");
 {
  CpuRenderer budget;budget.Initialize(1024,1024);
  const auto atlas=budget.RegisterUiMaterial(budget.Upload({2,2,8,white}));
  auto large=glyphs;for(auto &g:large){g.x0=g.y0=0;g.dx=0;g.pixelWidth=g.pixelHeight=255;}
  const auto largeFont=budget.RegisterFont(atlas,"budget-fixture",48,large);
  budget.BeginFrame();budget.Clear(color);budget.Submit();budget.Resize(1024,1024);
  const std::vector<uint8_t> original(budget.Pixels().bytes.begin(),budget.Pixels().bytes.end());
  budget.BeginFrame();budget.Clear(black);
  for(int command=0;command<3;++command)budget.DrawUiText(largeFont,placement,std::string(32,'A'),32,32,32,5,5,64,color);
  Check(Reject([&]{budget.Submit();})&&budget.Status()==CpuRenderer::State::recording&&budget.Presented()==0&&
        std::equal(original.begin(),original.end(),budget.Pixels().bytes.begin()),"whole-frame text budget rejects later command before any earlier clear mutates pixels");
 }
 std::vector<Glyph> extras(glyphs.begin(),glyphs.end());extras.push_back(glyphs[0]);extras.back().letter=1;
 r.Resize(80,80);const auto controlFont=r.RegisterFont(newMaterial,"control-metrics",48,extras);
 extras.push_back(extras.back());
 Check(controlFont!=0 && Reject([&]{r.RegisterFont(newMaterial,"bad-extra",48,extras);}),"genuine control glyph metrics accepted while duplicate extra ordering rejects");
 r.ReleaseFont(controlFont);
}catch(const std::exception&e){std::fprintf(stderr,"text fixture failed: %s\n",e.what());++failures;}
std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;}
