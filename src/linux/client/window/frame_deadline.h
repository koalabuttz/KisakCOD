#pragma once

#include <chrono>
#include <stdexcept>

namespace kisak::window {

// Presentation/host-tick pacing, independent of SDL event arrival. Initial
// presentation occurs before this timer starts. A late tick starts a new period;
// missed periods never generate catch-up bursts. This is not CL_Frame timing.
class FrameDeadline {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Milliseconds = std::chrono::milliseconds;

    explicit FrameDeadline(Milliseconds period=Milliseconds(16),
                           TimePoint start=Clock::now()) : period_(period)
    {
        if(period.count()<1 || period.count()>1000)
            throw std::invalid_argument("Frame period must be 1..1000 milliseconds");
        deadline_=Next(start);
    }

    int WaitMilliseconds(TimePoint now) const noexcept
    {
        if(now>=deadline_)return 0;
        // Cap before subtracting, including an accidentally earlier clock value.
        // The invariant deadline >= TimePoint::min()+period prevents overflow.
        if(now<=deadline_-period_)return static_cast<int>(period_.count());
        return static_cast<int>(std::chrono::ceil<Milliseconds>(deadline_-now).count());
    }

    bool TakeTick(TimePoint now)
    {
        if(now<deadline_)return false;
        deadline_=Next(now);
        return true;
    }

private:
    TimePoint Next(TimePoint now) const
    {
        if(now>TimePoint::max()-period_)
            throw std::overflow_error("Frame deadline exceeds steady clock range");
        return now+period_;
    }
    Milliseconds period_;
    TimePoint deadline_;
};
} // namespace kisak::window
