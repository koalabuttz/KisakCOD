// Synthetic resources isolate the actual production menu traversal. Genuine
// metadata/input/readback integration is checked separately with private assets.
#include <linux/client/window/cpu_renderer.h>
#include <ui/ui_shared.h>
#include <ui/ui_input_core.h>
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
 const float black[4]={0,0,0,1};auto start=[&]{r.CancelFrame();r.BeginFrame();r.Clear(black);};auto pixel=[&](int x,int y,int c){auto p=r.Pixels();return p.bytes[y*p.pitch+x*4+c];};
 dc.menuCount=1;dc.Menus[0]=&menu;
 dvar_s list{};list.type=DVAR_TYPE_BOOL;ui_showList=&list;
 start();auto result=r.DrawMenuFrame(roles,placement,dc,mats);r.Submit();
 Check(result.painted&&result.requestedBlur==7,"actual frame paints registered nonstacked menu");
 dc.menuStack[0]=&menu;dc.openMenuCount=1;dc.blurRadiusOut=99;start();r.DrawMenuFrame(roles,placement,dc,mats);r.Submit();
 Check(dc.blurRadiusOut==7,"stacked registered menu paints once and frame resets blur");
 menuDef_t second=menu;second.window.name="second";second.blurRadius=24;
 auto secondItems=items;std::array<itemDef_s*,2> secondPointers{&secondItems[0],&secondItems[1]};second.items=secondPointers.data();
 for(auto &i:secondItems){i.parent=&second;i.window.foreColor[0]=0;i.window.foreColor[1]=0;i.window.foreColor[2]=1;i.materialExp={};}
 dc.Menus[1]=&second;dc.menuCount=2;
 std::array<CpuRenderer::MenuMaterial,4> all{{mats[0],mats[1],{"white",mat,&secondItems[0]},{"white",mat,&secondItems[1]}}};
 auto draw=[&]{auto value=r.DrawMenuFrame(roles,placement,dc,all);r.Submit();return value;};
 start();draw();Check(pixel(5,5,1)==255&&dc.blurRadiusOut==25,"nonstacked menu precedes open stack irrespective of registry order");
 dc.menuStack[0]=&second;dc.menuStack[1]=&menu;dc.openMenuCount=2;start();draw();Check(pixel(5,5,1)==255&&dc.blurRadiusOut==25,"two stacked menus paint once in stack order");
 second.fullScreen=1;menu.fullScreen=1;start();draw();Check(pixel(5,5,1)==255&&dc.blurRadiusOut==7,"topmost fullscreen suppresses earlier fullscreen");
 menu.window.dynamicFlags[0]=0;start();result=draw();Check(!result.painted&&pixel(5,5,2)==0&&dc.blurRadiusOut==0,"hidden topmost fullscreen still suppresses lower menus");
 menu.window.dynamicFlags[0]=4;menu.fullScreen=second.fullScreen=0;dc.openMenuCount=0;
 start();draw();Check(pixel(5,5,2)==255&&dc.blurRadiusOut==25,"registered nonstacked source order is retained");
 dc.menuCount=0;auto savedItem=mats[0].item;mats[0].item=nullptr;start();dc.blurRadiusOut=83;result=r.DrawMenuFrame(roles,placement,dc,std::span<const CpuRenderer::MenuMaterial>(mats.data(),1));r.Submit();Check(!result.painted&&dc.blurRadiusOut==0,"empty registered frame resets blur without painting");dc.menuCount=2;mats[0].item=savedItem;
 menu.itemCount=second.itemCount=0;CpuRenderer::MenuMaterial namedOnly{"white",mat,nullptr};start();dc.blurRadiusOut=91;
 result=r.DrawMenuFrame(roles,placement,dc,{&namedOnly,1});r.Submit();
 Check(!result.painted&&result.requestedBlur==25&&result.postprocessUnsupported,"visible blur-only frame reports compositor request without emitted draw commands");menu.itemCount=second.itemCount=2;
 auto invalid=[&](auto change,auto restore,const char *name){start();dc.blurRadiusOut=19;change();auto prior=dc;auto before=items;Check(Reject([&]{r.DrawMenuFrame(roles,placement,dc,all);})&&!std::memcmp(&prior,&dc,sizeof(dc))&&!std::memcmp(before.data(),items.data(),sizeof(items)),name);restore();};
 invalid([&]{dc.menuStack[0]=nullptr;dc.openMenuCount=1;},[&]{dc.openMenuCount=0;},"null stack rejects before reset");
 menuDef_t foreign{};invalid([&]{dc.menuStack[0]=&foreign;dc.openMenuCount=1;},[&]{dc.openMenuCount=0;},"unregistered stack rejects atomically");
 invalid([&]{dc.menuStack[0]=dc.menuStack[1]=&menu;dc.openMenuCount=2;},[&]{dc.openMenuCount=0;},"duplicate stack rejects atomically");
 invalid([&]{dc.Menus[1]=&menu;},[&]{dc.Menus[1]=&second;},"duplicate registry pointer rejects");
 invalid([&]{second.window.name="SYNTHETIC-TRAVERSAL";},[&]{second.window.name="second";},"lookup-equivalent duplicate names reject");
 invalid([&]{captureFunc=[](UiContext*,void*){throw std::runtime_error("capture must not execute");};},[&]{captureFunc=nullptr;},"capture rejected before reset");
 invalid([&]{g_debugMode=1;},[&]{g_debugMode=0;},"unsupported debug overlay rejects");
 invalid([&]{list.current.enabled=true;},[&]{list.current.enabled=false;},"unsupported visible list rejects");
 invalid([&]{list.type=DVAR_TYPE_INT;},[&]{list.type=DVAR_TYPE_BOOL;},"wrong list dvar type rejects");
 invalid([&]{dc.menuCount=9;},[&]{dc.menuCount=2;},"excess registry work rejects");
 invalid([&]{secondItems[1].type=6;second.window.dynamicFlags[0]=0;},[&]{secondItems[1].type=0;second.window.dynamicFlags[0]=4;},"late hidden unsupported graph rejects before painting");
 start();for(int n=0;n<183;++n)r.DrawUiImage(mat,placement,0,0,1,1,5,5,nullptr);dc.blurRadiusOut=23;menu.window.rect.x=3;
 const auto oldItems=items,oldSecondItems=secondItems;const auto oldMenu=menu,oldSecond=second;const auto oldContext=dc;
 Check(Reject([&]{r.DrawMenuFrame(roles,placement,dc,all);})&&!std::memcmp(oldItems.data(),items.data(),sizeof(items))&&!std::memcmp(oldSecondItems.data(),secondItems.data(),sizeof(secondItems))&&!std::memcmp(&oldMenu,&menu,sizeof(menu))&&!std::memcmp(&oldSecond,&second,sizeof(second))&&!std::memcmp(&oldContext,&dc,sizeof(dc)),"late command exhaustion restores all menus items context");
 r.Submit();Check(r.Pixels().bytes[0]==255,"failed frame preserves prior command prefix");
 start();draw();Check(pixel(5,5,2)==255&&dc.blurRadiusOut==25,"scoped resources recover for later frame");
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL exception: %s\n",e.what());return 1;}}
