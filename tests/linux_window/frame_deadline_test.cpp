#include <linux/client/window/frame_deadline.h>
#include <cstdio>
#include <initializer_list>

namespace {
int checks=0,failures=0;
void Check(bool value,const char *name)
{
    ++checks;failures+=!value;
    std::printf("%s: %s\n",value?"PASS":"FAIL",name);
}
}

int main()
{
    using Timer=kisak::window::FrameDeadline;
    using namespace std::chrono;
    const Timer::TimePoint start{};
    for(int period:{-1,0,1001}) {
        bool rejected=false;
        try { Timer timer(milliseconds(period),start); }
        catch(const std::invalid_argument&) { rejected=true; }
        Check(rejected,"invalid frame period rejected");
    }
    Timer timer(16ms,start);
    Check(timer.WaitMilliseconds(start)==16 && !timer.TakeTick(start),
          "first tick follows initial presentation by one period");
    Check(timer.WaitMilliseconds(start+15ms+1ns)==1 && !timer.TakeTick(start+15ms+1ns),
          "fractional remaining time rounds up without an early tick");
    Check(timer.WaitMilliseconds(start+16ms)==0 && timer.TakeTick(start+16ms),
          "deadline permits one tick");
    unsigned premature=0;
    for(unsigned event=0;event<10000;++event)premature+=timer.TakeTick(start+17ms);
    Check(premature==0 && timer.WaitMilliseconds(start+17ms)==15,
          "event flood cannot create extra command/presentation ticks");
    Check(timer.TakeTick(start+1s) && !timer.TakeTick(start+1s) &&
          timer.WaitMilliseconds(start+1s)==16,
          "hitch produces one tick and no catch-up burst");
    Check(!timer.TakeTick(start+1015ms) && timer.TakeTick(start+1016ms),
          "pacing resumes one full period after hitch");
    Check(timer.TakeTick(start+hours(24)) && timer.WaitMilliseconds(start+hours(24))==16,
          "large monotonic jump reschedules without looping missed ticks");
    Check(timer.WaitMilliseconds(Timer::TimePoint::min())==16,
          "earlier supplied clock is bounded without overflowing subtraction");
    Timer fastest(1ms,start),slowest(1000ms,start);
    Check(fastest.TakeTick(start+1ms) && slowest.WaitMilliseconds(start)==1000,
          "supported minimum and maximum frame periods work");
    bool overflow=false;
    try { Timer impossible(16ms,Timer::TimePoint::max()); }
    catch(const std::overflow_error&) { overflow=true; }
    Check(overflow,"unrepresentable initial deadline fails explicitly");
    Timer limit(16ms,Timer::TimePoint::max()-16ms);
    overflow=false;
    try { limit.TakeTick(Timer::TimePoint::max()); }
    catch(const std::overflow_error&) { overflow=true; }
    Check(overflow,"unrepresentable next deadline fails without arithmetic overflow");
    std::printf("-- %d checks, %d failures --\n",checks,failures);
    return failures?1:0;
}
