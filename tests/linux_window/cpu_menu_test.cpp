// Synthetic metrics isolate genuine menu layout. Asset-backed proof is separate.
#include <linux/client/window/cpu_renderer.h>
#include <client/client.h>
#include <ui/ui_shared.h>
#include <array>
#include <cstdio>
#include <cmath>
#include <vector>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char*n){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",n);}
template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}
bool Lit(CpuRenderer&r,int x,int y){auto p=r.Pixels();return p.bytes[y*p.pitch+x*4]>0;}
}
int main(){try{
 CpuRenderer r;r.Initialize(160,160);ScreenPlacement placement{};placement.scaleVirtualToReal[0]=placement.scaleVirtualToReal[1]=1;
 std::array<uint8_t,16> white;white.fill(255);auto material=r.RegisterUiMaterial(r.Upload({2,2,8,white}));
 std::array<Glyph,96> glyphs{};for(unsigned i=0;i<96;++i){auto&g=glyphs[i];g.letter=i+32;g.x0=0;g.y0=-2;g.dx=4;g.pixelWidth=g.pixelHeight=2;g.s1=g.t1=1;}
 glyphs[0].pixelWidth=glyphs[0].pixelHeight=0;
 auto font=r.RegisterFont(material,"menu-fixture",48,glyphs);CpuRenderer::MenuFonts roles;roles.console=font;
 itemDef_s item{};item.fontEnum=5;item.textscale=1;item.window.rect={4,8,12,100,5,5};
 const float black[4]={0,0,0,1},red[4]={1,0,0,1};
 auto draw=[&](std::string_view text){r.Resize(160,160);r.BeginFrame();r.Clear(black);r.DrawMenuText(roles,placement,item,text,red);r.Submit();};
 draw("AA AA");Check(Lit(r,3,5)&&Lit(r,7,5)&&Lit(r,3,58)&&!Lit(r,11,5),"actual word wrap copies two temporary lines and uses font height plus five spacing");
 Check(item.textRect[0].x==4&&item.textRect[0].w==8&&item.textRect[0].h==106,"actual painter writes measured multiline text rectangle");
 item.textAlignMode=1;draw("AA AA");Check(item.textRect[0].x==6&&Lit(r,5,5),"actual centered wrapped alignment");
 item.textAlignMode=2;draw("AA AA");Check(item.textRect[0].x==8&&Lit(r,7,5),"actual right wrapped alignment");
 item.textAlignMode=4;draw("A");Check(item.textRect[0].y==8&&Lit(r,3,53),"top alignment places baseline using actual text height");
 item.textAlignMode=0;item.window.border=1;item.window.borderSize=2;draw("A");Check(item.textRect[0].x==6&&Lit(r,5,7),"actual ToWindowCoords applies border and item origin");
 item.window.border=0;draw("");Check(item.textRect[0].w==0&&item.textRect[0].h==0&&!Lit(r,3,5),"empty item produces no commands and empty extents");
 draw("AAAAAAAA");Check(item.textRect[0].w==32&&Lit(r,31,5),"unbreakable word preserves actual overflow behavior rather than inventing item clipping");
 draw("AA\nA");Check(Lit(r,3,58),"actual explicit newline wrapping");
 r.Resize(160,160);r.BeginFrame();r.Clear(black);const auto oldRect=item.textRect[0];
 Check(Reject([&]{r.DrawMenuText(roles,placement,item,"good\tbad",red);})&&item.textRect[0].w==oldRect.w,"unsupported late control rejects before item mutation");
 item.textStyle=128;Check(Reject([&]{r.DrawMenuText(roles,placement,item,"A",red);}),"unsupported menu style rejects explicitly");item.textStyle=0;
 item.window.rect.w=.001f;Check(Reject([&]{r.DrawMenuText(roles,placement,item,"A A",red);}),"subpixel wrap width rejects before integer layout conversion");item.window.rect.w=12;
 std::string many;for(int i=0;i<140;++i)many+="A\n";
 Check(Reject([&]{r.DrawMenuText(roles,placement,item,many,red);})&&item.textRect[0].w==oldRect.w,"late line command-capacity exhaustion rolls back entire item recording and extents");
 r.DrawMenuText(roles,placement,item,"A",red);r.Submit();Check(Lit(r,3,5)&&!Lit(r,3,58),"post-failure draw succeeds with no surviving partial lines");
 r.Resize(160,160);r.BeginFrame();item.fontEnum=3;
 Check(Reject([&]{r.DrawMenuText(roles,placement,item,"A",red);}),"missing exact selected font role fails without fallback");
 roles.small=font;r.DrawMenuText(roles,placement,item,"A",red);r.Submit();Check(Lit(r,3,5),"explicit small-font enum follows production role selection");
 r.Resize(160,160);r.BeginFrame();item.fontEnum=0;item.textscale=.3f;
 Check(Reject([&]{r.DrawMenuText(roles,placement,item,"A",red);}),"automatic intermediate scale requires real text-font role");
 roles.text=font;r.DrawMenuText(roles,placement,item,"A",red);r.Submit();Check(item.textRect[0].w==1,"automatic font threshold path uses actual normalized metrics");

 r.Resize(160,160);r.BeginFrame();const float background[4]={1,1,1,1},halfRed[4]={1,0,0,.5f};r.Clear(background);
 item.fontEnum=5;item.textscale=1;item.textStyle=3;r.DrawMenuText(roles,placement,item,"A",halfRed);r.Submit();
 auto channel=[&](int x,int y,int c){auto p=r.Pixels();return p.bytes[y*p.pitch+x*4+c];};
 Check(channel(5,7,0)==127&&channel(5,7,1)==127&&channel(5,7,2)==127,"actual style3 shadow uses original alpha over nonblack background");
 Check(channel(3,5,0)==255&&channel(3,5,1)==127&&channel(6,7,0)==255,"shadow is black one physical pixel behind foreground with no expanded extent");
 r.Resize(160,160);r.BeginFrame();r.Clear(background);item.textscale=2;r.DrawMenuText(roles,placement,item,"A",halfRed);r.Submit();
 Check(channel(7,7,0)==127&&channel(8,7,0)==255,"style3 offset remains one physical pixel at doubled glyph scale");
 r.Resize(160,160);auto strictMaterial=r.RegisterUiMaterial(r.Upload({2,2,8,white}),true);
 auto strictFont=r.RegisterFont(strictMaterial,"metadata-fixture",48,glyphs);auto strictRoles=roles;strictRoles.console=strictFont;
 const std::vector<uint8_t> previous(r.Pixels().bytes.begin(),r.Pixels().bytes.end());
 r.BeginFrame();r.Clear(black);item.textscale=.5f;r.DrawMenuText(strictRoles,placement,item,"A",red);
 Check(Reject([&]{r.Submit();})&&std::equal(previous.begin(),previous.end(),r.Pixels().bytes.begin()),"metadata font minification including shadow rejects before preceding clear mutation");
 r.Resize(160,160);r.BeginFrame();r.Clear(black);r.DrawUiImage(strictMaterial,placement,0,0,1,1,5,5,red);
 Check(Reject([&]{r.Submit();})&&std::equal(previous.begin(),previous.end(),r.Pixels().bytes.begin()),"metadata image minification rejects atomically");
 r.Resize(160,160);r.BeginFrame();r.Clear(black);item.textscale=1;r.DrawMenuText(strictRoles,placement,item,"A",red);r.Submit();
 Check(Lit(r,3,5),"metadata-backed equal-size sampling remains supported");
 r.Shutdown();r.Initialize(160,160);r.BeginFrame();Check(Reject([&]{r.DrawMenuText(roles,placement,item,"A",red);}),"restart rejects stale role handles");
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"menu test: %s\n",e.what());return 2;}}
