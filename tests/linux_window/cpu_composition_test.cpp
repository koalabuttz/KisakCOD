// Synthetic resources isolate the real painter; genuine asset readback is separate.
#include <linux/client/window/cpu_renderer.h>
#include <ui/ui_shared.h>
#include <ui/ui_cursor.h>
#include <client/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace kisak::window;
namespace {int checks=0,failures=0;void Check(bool x,const char*n){++checks;failures+=!x;std::printf("%s: %s\n",x?"PASS":"FAIL",n);}template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}}
int main(){try{
 CpuRenderer r;r.Initialize(64,64);std::array<uint8_t,16> white;white.fill(255);auto m=r.RegisterUiMaterial(r.Upload({2,2,8,white}));
 ScreenPlacement p{};for(int i=0;i<2;++i){p.scaleVirtualToReal[i]=1;p.scaleVirtualToFull[i]=1;}UiContext dc{};
 windowDef_t w{};w.style=3;w.rect={4,5,8,6,5,5};w.dynamicFlags[0]=0x10000;w.foreColor[0]=1;w.foreColor[3]=1;
 const float black[4]={0,0,0,1};auto start=[&]{r.CancelFrame();r.BeginFrame();r.Clear(black);};auto red=[&](int x,int y){auto v=r.Pixels();return v.bytes[y*v.pitch+x*4];};
 start();r.DrawWindow(m,p,dc,w,.1f,.1f,1,10);r.Submit();Check(red(4,5)==255&&red(11,10)==255&&red(12,10)==0,"actual style3 painter uses geometry and enabled forecolor");
 w.dynamicFlags[0]=0;start();r.DrawWindow(m,p,dc,w,.1f,.1f,1,10);r.Submit();auto v=r.Pixels();Check(v.bytes[5*v.pitch+4*4+1]==255,"style3 without forecolor flag uses default white");
 w.style=1;w.dynamicFlags[0]=0x20;w.backColor[0]=1;w.backColor[3]=.25f;w.nextTime=100;dc.realTime=100;start();r.DrawWindow(m,p,dc,w,.1f,.25f,.75f,10);Check(w.backColor[3]==.25f&&w.nextTime==100,"Fade strict deadline does not advance at equality");
 dc.realTime=101;r.DrawWindow(m,p,dc,w,.1f,.25f,.75f,10);Check(w.backColor[3]==.5f&&w.nextTime==111,"real Fade advances once and publishes mutation");dc.realTime=1000;r.DrawWindow(m,p,dc,w,.1f,.25f,.75f,10);Check(w.backColor[3]==.75f&&!(w.dynamicFlags[0]&0x20),"Fade clamps and clears actual fade-in flag after hitch");
 w.foreColor[0]=std::numeric_limits<float>::quiet_NaN();const auto saved=w.nextTime;Check(Reject([&]{r.DrawWindow(m,p,dc,w,.1f,.1f,1,10);})&&w.nextTime==saved,"invalid painter input leaves window timing unchanged");w.foreColor[0]=1;
 w.border=1;Check(Reject([&]{r.DrawWindow(m,p,dc,w,.1f,.1f,1,10);}),"unsupported border fails before recording");w.border=0;w.style=6;Check(Reject([&]{r.DrawWindow(m,p,dc,w,.1f,.1f,1,10);}),"loadbar unavailable explicitly");w.style=3;r.DrawWindow(m,p,dc,w,.1f,.1f,1,10);r.Submit();Check(red(4,5)==255,"failed scopes restore subsequent real painter");
 UI_UpdateCursorPosition(&dc,&p,-1,10);Check(dc.cursor.x==-1&&!dc.isCursorVisible,"real cursor retains negative coordinate and hides");UI_UpdateCursorPosition(&dc,&p,640,480);Check(dc.isCursorVisible,"inclusive real cursor bounds");UI_UpdateCursorPosition(&dc,&p,641,480);Check(!dc.isCursorVisible,"outside real cursor bound hides");
 dc.cursor={32,32};dc.isCursorVisible=1;start();r.DrawCursor(m,p,p,dc);r.Submit();Check(red(16,16)==255&&red(47,47)==255&&red(15,16)==0&&red(48,47)==0,"actual cursor centered32 virtual pixel geometry");
 dc.isCursorVisible=0;start();r.DrawCursor(m,p,p,dc);r.Submit();Check(red(32,32)==0,"hidden cursor records no draw");
 p.scaleVirtualToReal[0]=2;p.scaleVirtualToFull[0]=2;dc.cursor={16,32};dc.isCursorVisible=1;start();r.DrawCursor(m,p,p,dc);r.Submit();Check(red(0,16)==255&&red(63,47)==255,"cursor physical width follows actual real/full placement ratio");
 r.CancelFrame();std::array<uint8_t,8> rb{255,0,0,255,0,0,255,255};auto flipped=r.RegisterUiMaterial(r.Upload({2,1,8,rb}));
 w.style=3;w.dynamicFlags[0]=0;w.rect={4,5,-8,-6,5,5};start();r.DrawWindow(flipped,p,dc,w,.1f,.1f,1,10);r.Submit();auto flipPixels=r.Pixels();
 Check(red(4,5)==0&&flipPixels.bytes[5*flipPixels.pitch+4*4+2]==255&&red(11,5)==255,"actual negative-size window caps preserve UI UV mirroring");w.rect={4,5,8,6,5,5};
 start();w.style=1;w.dynamicFlags[0]=0x20;w.backColor[3]=0;w.nextTime=0;bool exhausted=false,unchanged=false;
 for(int i=1;i<400;++i){dc.realTime=i*20;const auto prior=w;try{r.DrawWindow(m,p,dc,w,.01f,.001f,1,10);}catch(const std::exception&){exhausted=true;unchanged=w.nextTime==prior.nextTime&&w.backColor[3]==prior.backColor[3]&&w.dynamicFlags[0]==prior.dynamicFlags[0];break;}}
 Check(exhausted&&unchanged,"late command capacity failure rolls back actual Fade mutation");
 r.CancelFrame();start();dc.isCursorVisible=1;r.DrawCursor(m,p,p,dc);r.Submit();Check(red(32,32)==255,"capacity failure restores material scope for cursor");
 p.scaleVirtualToReal[0]=p.scaleVirtualToFull[0]=1;dc.cursor={0,0};dc.isCursorVisible=1;
 start();r.SetViewport(3,4,8,9);r.DrawCursor(m,p,p,dc);r.Submit();bool clipped=true;
 for(int y=0;y<64;++y)for(int x=0;x<64;++x)clipped&=red(x,y)==((x>=3&&x<11&&y>=4&&y<13)?255:0);
 Check(clipped,"actual cursor gains nonzero viewport origin and clips every target pixel");
 dc.cursor={32,32};start();r.DrawCursor(m,p,p,dc);r.Submit();
 Check(red(16,16)==255&&red(47,47)==255&&red(3,4)==0,"next cursor command list resets prior subviewport to full target");
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL exception: %s\n",e.what());return 1;}}
