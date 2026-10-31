// Synthetic resources isolate the actual production menu traversal. Genuine
// metadata/input/readback integration is checked separately with private assets.
#include <linux/client/window/cpu_renderer.h>
#include <ui/ui_shared.h>
#include <client/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace kisak::window;
namespace {int checks,failures;void Check(bool v,const char*n){++checks;failures+=!v;std::printf("%s: %s\n",v?"PASS":"FAIL",n);}template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}}
int main(){try{
 dvar_s show{},debug{};show.type=DVAR_TYPE_STRING;show.current.string="";ui_showMenuOnly=&show;uiscript_debug=&debug;
 CpuRenderer r;r.Initialize(32,32);std::array<uint8_t,16> white;white.fill(255);auto mat=r.RegisterUiMaterial(r.Upload({2,2,8,white}));
 std::array<Glyph,96> glyphs{};for(unsigned n=0;n<96;++n){glyphs[n].letter=n+32;glyphs[n].dx=1;}auto font=r.RegisterFont(mat,"synthetic-traversal",16,glyphs);
 CpuRenderer::MenuFonts roles;roles.small=roles.text=roles.big=roles.extraBig=roles.bold=roles.console=roles.objective=font;
 ScreenPlacement placement{};placement.scaleVirtualToReal[0]=placement.scaleVirtualToReal[1]=1;placement.scaleVirtualToFull[0]=placement.scaleVirtualToFull[1]=1;
 UiContext dc{};menuDef_t menu{};menu.window.name="synthetic-traversal";menu.window.rect={0,0,32,32,5,5};menu.window.dynamicFlags[0]=4;menu.blurRadius=7;menu.fadeClamp=1;
 std::array<itemDef_s,2> items{};std::array<itemDef_s*,2> pointers{&items[0],&items[1]};menu.items=pointers.data();menu.itemCount=2;
 for(int n=0;n<2;++n){auto&i=items[n];i.parent=&menu;i.window.style=3;i.window.staticFlags=0x100000;i.window.dynamicFlags[0]=0x10004;i.window.rectClient={float(n*4),float(n*4),12,12,5,5};i.window.rect=i.window.rectClient;i.window.foreColor[n]=1;i.window.foreColor[3]=1;}
 std::array<expressionEntry,2> expr{};expr[0].type=0;expr[0].data.op=OP_LEFTPAREN;expr[1].type=1;expr[1].data.operand.dataType=VAL_STRING;expr[1].data.operand.internals.string="white";std::array<expressionEntry*,2> ep{&expr[0],&expr[1]};items[0].materialExp={2,ep.data()};
 std::array<CpuRenderer::MenuMaterial,2> mats{{{"white",mat,&items[0]},{"white",mat,&items[1]}}};
 const float black[4]={0,0,0,1};auto start=[&]{r.CancelFrame();r.BeginFrame();r.Clear(black);dc.blurRadiusOut=0;};auto pixel=[&](int x,int y,int c){auto p=r.Pixels();return p.bytes[y*p.pitch+x*4+c];};
 start();auto result=r.DrawMenuTraversal(roles,placement,dc,menu,mats);r.Submit();
 Check(result.painted&&result.requestedBlur==7&&result.postprocessUnsupported,"real traversal reports actual unrendered blur request");
 Check(pixel(1,1,0)==255&&pixel(5,5,1)==255&&pixel(5,5,0)==0,"actual source-order item traversal controls overlapping pixels");
 Check(items[0].window.background&&items[0].window.background==items[1].window.background,"actual literal material expression resolves shared owned name identity");
 items[1].window.dynamicFlags[0]&=~4;start();r.DrawMenuTraversal(roles,placement,dc,menu,mats);r.Submit();Check(pixel(5,5,0)==255&&pixel(5,5,1)==0,"real Item_IsVisible suppresses hidden item");items[1].window.dynamicFlags[0]|=4;
 menu.window.rect.x=3;menu.window.rect.y=2;items[0].textRect[0].w=99;start();r.DrawMenuTraversal(roles,placement,dc,menu,mats);r.Submit();Check(items[0].window.rect.x==3&&items[0].window.rect.y==2&&items[0].textRect[0].w==0&&pixel(3,2,0)==255&&pixel(2,2,0)==0,"real Menu_UpdatePosition applies parent origin and invalidates cached extents");
 start();r.DrawMenuTraversal(roles,placement,dc,menu,mats);Check(dc.blurRadiusOut==7,"independent frame resets blur accumulation");r.DrawMenuTraversal(roles,placement,dc,menu,mats);Check(dc.blurRadiusOut>9.89f&&dc.blurRadiusOut<9.91f,"multiple actual menus within one frame accumulate squared blur requests");r.CancelFrame();
 menu.window.dynamicFlags[0]&=~4;start();result=r.DrawMenuTraversal(roles,placement,dc,menu,mats);r.Submit();Check(!result.painted&&result.requestedBlur==0&&pixel(4,4,0)==0,"real hidden menu returns unpainted without compositor request");menu.window.dynamicFlags[0]|=4;
 show.current.string="different-menu";start();result=r.DrawMenuTraversal(roles,placement,dc,menu,mats);r.Submit();Check(!result.painted,"actual ui_showMenuOnly string gate retained");show.current.string="";
 auto before=std::vector<uint8_t>(r.Pixels().bytes.begin(),r.Pixels().bytes.end());auto first=items[0];items[1].type=6;items[1].window.dynamicFlags[0]&=~4;start();Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);})&&std::memcmp(&first,&items[0],sizeof(first))==0&&std::equal(before.begin(),before.end(),r.Pixels().bytes.begin()),"unsupported late hidden item rejects before prefix mutation");items[1].type=0;items[1].window.dynamicFlags[0]|=4;
 expr[1].data.operand.internals.string="unregistered";Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);}),"literal name outside owned resolver rejects before dispatch");expr[1].data.operand.internals.string="white";
 menu.window.rect.horzAlign=99;Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);}),"invalid inherited parent alignment rejects before production assertions");menu.window.rect.horzAlign=5;
 items[1].type=items[1].dataType=1;items[1].fontEnum=1;items[1].textscale=1;items[1].textAlignMode=3;Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);}),"invalid hidden-capable text alignment is preflight rejected");items[1].type=items[1].dataType=0;items[1].textAlignMode=0;
 start();for(int n=0;n<185;++n)r.DrawUiImage(mat,placement,0,0,1,1,5,5,nullptr);menu.window.rect.x=7;const auto prior0=items[0],prior1=items[1];const auto priorBlur=dc.blurRadiusOut;
 Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);})&&std::memcmp(&prior0,&items[0],sizeof(prior0))==0&&std::memcmp(&prior1,&items[1],sizeof(prior1))==0&&dc.blurRadiusOut==priorBlur,"late command-capacity failure restores all item state and blur accumulator");
 r.Submit();Check(pixel(0,0,0)==255,"failed traversal preserves previously recorded command prefix");
 start();r.DrawMenuTraversal(roles,placement,dc,menu,mats);r.Submit();Check(items[0].window.rect.x==7,"failed traversal restores scopes for a later successful frame");
 start();show.type=DVAR_TYPE_BOOL;Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);}),"incorrect menu-filter dvar type fails before union dereference");show.type=DVAR_TYPE_STRING;
 show.current.string=nullptr;Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);}),"null menu-filter string is rejected before production paint");show.current.string="";
 std::array<char,256> unterminated;unterminated.fill('x');const auto *savedName=menu.window.name;menu.window.name=unterminated.data();Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);}),"unbounded menu name rejected before profiling strlen");menu.window.name=savedName;
 items[1].type=items[1].dataType=1;items[1].window.rectClient.w=-1;Check(Reject([&]{r.DrawMenuTraversal(roles,placement,dc,menu,mats);}),"nondecorative text geometry rejects mirrored-width layout before production");items[1].type=items[1].dataType=0;items[1].window.rectClient.w=12;
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL exception: %s\n",e.what());return 1;}}
