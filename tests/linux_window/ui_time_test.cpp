// Asset-free execution of shared production UI clock and actual Fade.
#include <ui/ui_time.h>
#include <ui/ui_shared.h>
#include <array>
#include <cstdio>
#include <limits>
#include <bit>
#include <cstdint>
namespace {
int checks,failures;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
struct Clock {
 UiContext context{};int index=0;int history[4]{};
 void Tick(int time){UI_UpdateContextTime(&context,&index,history,time);}
};
}
int main(){
 Clock a;
 a.context.FPS=123;
 a.Tick(10);a.Tick(30);a.Tick(60);a.Tick(100);
 Check(a.context.realTime==100&&a.context.frameTime==40&&a.index==4,"real UI timestamps and frame delta update once per sample");
 Check(a.context.FPS==123,"production four-sample warmup preserves previous FPS");
 Check(a.history[0]==10&&a.history[1]==20&&a.history[2]==30&&a.history[3]==40,"warmup retains ordered four-frame history");
 a.Tick(150);
 Check(a.context.FPS==28&&a.history[0]==50&&a.index==5,"fifth frame replaces oldest sample and truncates FPS division");
 a.Tick(150);
 Check(a.context.frameTime==0&&a.history[1]==0&&a.context.FPS==33,"repeated timestamp records a zero frame without inventing elapsed time");
 a.Tick(140);
 Check(a.context.frameTime==-10&&a.history[2]==-10&&a.context.FPS==50,"ordinary backward timestamp preserves production signed delta behavior");
 Clock still;for(int i=0;i<5;++i)still.Tick(0);
 Check(still.context.FPS==4000,"zero history denominator uses the existing one-millisecond guard");
 Clock independent;independent.Tick(7);
 Check(independent.index==1&&independent.context.FPS==0&&a.index==7,"independent owner clock does not borrow another history");
 independent=Clock{};independent.Tick(9);
 Check(independent.index==1&&independent.context.realTime==9&&independent.history[1]==0,"recreated clock starts with clean history");
 Clock wrapped;wrapped.context.realTime=std::numeric_limits<int>::max()-4;
 wrapped.Tick(std::numeric_limits<int>::min()+5);
 Check(wrapped.context.frameTime==10,"32-bit clock crossing signed boundary retains ten elapsed milliseconds");
 wrapped.context.realTime=-4;wrapped.Tick(6);
 Check(wrapped.context.frameTime==10,"full unsigned clock wrap retains ten elapsed milliseconds");
 Clock counter;counter.index=std::numeric_limits<int>::max();for(int&i:counter.history)i=16;
 counter.Tick(16);
 Check(counter.index==4&&counter.context.FPS==62&&counter.history[3]==16,"saturated lifetime counter keeps warmed ring phase and FPS");
 counter.Tick(36);
 Check(counter.index==5&&counter.history[0]==20&&counter.context.FPS==58,"frame after lifetime-counter rollover replaces slot zero");
 Clock longFrames;for(uint32_t time:{1000000000u,2000000000u,3000000000u,4000000000u,705032704u})longFrames.Tick(std::bit_cast<int>(time));
 Check(longFrames.context.frameTime==1000000000&&longFrames.context.FPS==0,"wide history sum admits long frames without signed overflow");
 // Time is sampled at the frame boundary; actual Fade retains its strict >
 // threshold and advances once per call, not once per missed timer interval.
 Clock fading;int flags=0x24,nextTime=100;float alpha=0;
 auto paint=[&]{Fade(&flags,&alpha,1,&nextTime,10,1,.25f,.25f,&fading.context);};
 fading.Tick(100);paint();
 Check(alpha==0&&nextTime==100&&flags==0x24,"real Fade does not advance at exact timer boundary");
 fading.Tick(101);paint();
 Check(alpha==.25f&&nextTime==111&&flags==0x24,"real Fade uses sampled UI frame time to advance once");
 paint();
 Check(alpha==.25f&&fading.index==2,"extra paint does not resample time or advance same-frame fade");
 fading.Tick(1001);paint();
 Check(alpha==.5f&&nextTime==1011,"long frame advances actual Fade once without artificial catchup");
 fading.Tick(1012);paint();fading.Tick(1023);paint();
 Check(alpha==1&&flags==4,"actual fade-in clamps alpha and clears its transition flag");
 flags=0x14;alpha=.25f;nextTime=1023;fading.Tick(1024);paint();
 Check(alpha==0&&flags==0,"actual fade-out removes visibility at zero alpha");
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
