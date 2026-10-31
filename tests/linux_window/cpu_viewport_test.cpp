#include <linux/client/window/cpu_renderer.h>
#include <client/client.h>
#include <gfx_d3d/r_state.h>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <climits>
using namespace kisak::window;
namespace {int checks,failures;void Check(bool b,const char*s){++checks;failures+=!b;std::printf("%s: %s\n",b?"PASS":"FAIL",s);}template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
GfxCmdHeader *Append(void*p,GfxRenderCommand id,int bytes){return R_AppendCommand(*static_cast<GfxCmdArray*>(p),256,id,bytes);}
bool Pixel(const CpuRenderer&r,int x,int y,std::array<uint8_t,4> c){auto p=r.Pixels();return std::equal(c.begin(),c.end(),p.bytes.begin()+y*p.pitch+4*x);}
std::vector<uint8_t> Bytes(const CpuRenderer&r){auto p=r.Pixels();return {p.bytes.begin(),p.bytes.end()};}}
int main(){try{
 static_assert(sizeof(GfxCmdSetViewport)==20);
 GfxCmdBufSourceState state{};state.viewportBehavior=GFX_USE_VIEWPORT_FOR_VIEW;state.viewMode=VIEW_MODE_3D;
 R_SetViewportValues(&state,3,2,5,4);
 Check(state.sceneViewport.x==3&&state.sceneViewport.y==2&&state.sceneViewport.width==5&&state.sceneViewport.height==4&&state.viewMode==VIEW_MODE_NONE&&state.viewportIsDirty,"actual viewport state setter invalidates view mode and viewport cache");
 alignas(4)std::array<uint8_t,256> commands{};GfxCmdArray list{commands.data(),0,0,nullptr};R_EmitSetViewportCommand(&list,Append,3,2,5,4);
 GfxCmdSetViewport packed;std::memcpy(&packed,commands.data(),sizeof(packed));
 Check(packed.header.id==RC_SET_VIEWPORT&&packed.header.byteCount==20&&!std::memcmp(&packed.viewport,&state.sceneViewport,sizeof(GfxViewport)),"shared producer preserves exact engine viewport command ABI");
 Check(Reject([]{CL_SetViewport(0,0,1,1);}),"unscoped actual client viewport entry fails explicitly");
 CpuRenderer r;r.Initialize(12,8);std::array<uint8_t,4> red={255,0,0,255};auto texture=r.Upload({1,1,4,red});auto material=r.RegisterUiMaterial(texture);
 ScreenPlacement placement{};const float gray[4]={.25f,.5f,.75f,1},white[4]={1,1,1,1},black[4]={0,0,0,1};
 r.BeginFrame();r.Clear(gray);r.SetViewport(3,2,5,4);r.DrawUiImage(material,placement,-1,-1,7,6,5,5,white);r.Submit();
 bool exact=true;for(int y=0;y<8;++y)for(int x=0;x<12;++x)exact&=Pixel(r,x,y,x>=3&&x<8&&y>=2&&y<6?red:std::array<uint8_t,4>{64,128,191,255});
 Check(exact,"actual client viewport origin and all four clip edges affect UI pixels");
 r.CancelFrame();r.BeginFrame();r.Clear(black);r.DrawUiImage(material,placement,0,0,1,1,5,5,white);r.Submit();
 Check(Pixel(r,0,0,red)&&Pixel(r,3,2,{0,0,0,255}),"new command list resets full viewport without explicit viewport command");
 r.CancelFrame();r.BeginFrame();r.Clear(black);r.SetViewport(3,2,5,4);r.DrawUiImage(material,placement,.5f,.5f,1,1,5,5,white);r.SetViewport(9,0,2,2);r.DrawUiImage(material,placement,0,0,1,1,5,5,white);r.Submit();
 Check(Pixel(r,3,2,red)&&Pixel(r,4,2,{0,0,0,255})&&Pixel(r,3,3,{0,0,0,255}),"fractional half-open coverage retains existing pixel-center convention");
 Check(Pixel(r,9,0,red)&&Pixel(r,10,0,{0,0,0,255}),"multiple actual viewport commands affect only subsequent draws");
 r.CancelFrame();r.BeginFrame();
 for(auto v:std::array<GfxViewport,6>{{{-1,0,1,1},{0,-1,1,1},{0,0,0,1},{0,0,1,0},{INT_MAX,0,2,1},{10,7,3,2}}})Check(Reject([&]{r.SetViewport(v.x,v.y,v.width,v.height);}),"invalid viewport rejects before production assertion or recording");
 r.SetViewport(0,0,12,8);r.DrawUiImage(material,placement,0,0,1,1,5,5,white);r.Submit();Check(Pixel(r,0,0,red),"failed viewport scope restores for subsequent actual client call");
 r.CancelFrame();const auto snapshot=Bytes(r);const auto count=r.Presented();
 auto malformed=[&](GfxViewport viewport,int size,bool truncate){list={commands.data(),0,0,nullptr};R_EmitClearScreenCommand(&list,Append,1,white,1,0);R_EmitSetViewportCommand(&list,Append,0,0,1,1);GfxCmdSetViewport bad{};std::memcpy(&bad,commands.data()+sizeof(GfxCmdClearScreen),sizeof(bad));bad.viewport=viewport;bad.header.byteCount=size;std::memcpy(commands.data()+sizeof(GfxCmdClearScreen),&bad,sizeof(bad));R_EmitEndCommand(&list,Append);return Reject([&]{r.SubmitCommands({commands.data(),size_t(list.usedTotal-(truncate?7:0))});})&&Bytes(r)==snapshot&&r.Status()==CpuRenderer::State::ready&&r.Presented()==count;};
 Check(malformed({-1,0,1,1},20,false),"late negative viewport rejects entire list before earlier clear");
 Check(malformed({0,0,13,8},20,false),"out-of-target viewport rejects atomically");
 Check(malformed({0,0,1,1},16,false),"wrong viewport command size rejects atomically");
 Check(malformed({0,0,1,1},20,true),"truncated viewport command rejects atomically");
 r.BeginFrame();r.SetViewport(3,2,5,4);r.Clear(white);Check(Reject([&]{r.Submit();})&&Bytes(r)==snapshot,"ambiguous clear after subviewport rejects before pixel mutation");r.CancelFrame();
 r.BeginFrame();r.SetViewport(3,2,5,4);r.Blit(texture,0,0,1,1);Check(Reject([&]{r.Submit();})&&Bytes(r)==snapshot,"raw StretchRect copy under subviewport rejects explicitly");r.CancelFrame();
 std::array<Glyph,96> glyphs{};for(int i=0;i<96;++i)glyphs[i]={};for(int i=0;i<96;++i){auto &g=glyphs[i];g.letter=i+32;g.dx=1;g.pixelWidth=1;g.pixelHeight=1;g.s1=g.t1=1;}
 auto font=r.RegisterFont(material,"viewport-test",48,glyphs);r.BeginFrame();r.Clear(black);r.SetViewport(3,2,2,1);r.DrawUiText(font,placement,"AAA",3,.5f,.5f,5,5,1,white);r.Submit();
 Check(Pixel(r,3,2,red)&&Pixel(r,4,2,red)&&Pixel(r,5,2,{0,0,0,255}),"real text glyph replay shares viewport origin and clipping");
 r.Resize(16,10);r.BeginFrame();r.Clear(black);r.DrawUiImage(material,placement,15,9,1,1,5,5,white);r.Submit();Check(Pixel(r,15,9,red),"resize resets full target viewport while retaining owned material resources");
 r.CancelFrame();std::array<uint8_t,16> quad={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
 auto quadMaterial=r.RegisterUiMaterial(r.Upload({2,2,8,quad}));r.BeginFrame();r.Clear(black);r.SetViewport(3,2,2,2);r.DrawUiImage(quadMaterial,placement,-1,-1,4,4,5,5,white);r.Submit();
 Check(Pixel(r,3,2,{159,64,64,255})&&Pixel(r,4,2,{96,191,64,255})&&Pixel(r,3,3,{96,64,191,255})&&Pixel(r,4,3,{159,191,191,255}),"clipping retains original nonuniform texture UV sampling without rescaling");
 r.CancelFrame();const auto policySnapshot=Bytes(r);r.BeginFrame();r.SetViewport(3,2,2,2);r.SetViewport(0,0,16,10);r.Clear(white);
 Check(Reject([&]{r.Submit();})&&Bytes(r)==policySnapshot,"reset to full viewport does not make earlier dirty-viewport clear ordering supported");r.CancelFrame();
 r.BeginFrame();r.SetViewport(3,2,2,2);r.SetViewport(0,0,16,10);r.Blit(texture,0,0,1,1);r.Submit();
 Check(Pixel(r,0,0,red),"raw absolute copy is admitted after explicit full viewport restore");
 r.Shutdown();r.Initialize(2,2);r.BeginFrame();r.Clear(gray);r.SetViewport(0,0,2,2);r.Submit();Check(Pixel(r,1,1,{64,128,191,255}),"shutdown/restart discards prior viewport state");
 }catch(const std::exception&e){std::fprintf(stderr,"viewport fixture: %s\n",e.what());++failures;}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;}
