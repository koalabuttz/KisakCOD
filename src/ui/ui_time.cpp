#include "ui_time.h"
#include "ui_shared.h"
#include <bit>
#include <cstdint>
#include <limits>

void UI_UpdateContextTime(UiContext *dc,int *timeIndex,int previousTimes[4],int realtime)
{
    static_assert(sizeof(int)==sizeof(uint32_t));
    // Engine millisecond clocks wrap in 32 bits. Unsigned subtraction retains
    // that elapsed-time interpretation without overflowing signed int at 2^31.
    dc->frameTime = std::bit_cast<int>(uint32_t(realtime)-uint32_t(dc->realTime));
    dc->realTime = realtime;
    const bool historyReady = *timeIndex >= 4;
    previousTimes[uint32_t(*timeIndex) % 4] = dc->frameTime;
    // Keep the original warmup and ring phase when the lifetime count fills.
    // INT_MAX occupies slot 3; 4 resumes at slot 0 with a warmed history.
    *timeIndex = *timeIndex==std::numeric_limits<int>::max()?4:*timeIndex+1;
    if (historyReady)
    {
        int64_t frameTimeTotal = 0;
        for (int index = 0; index < 4; ++index)
            frameTimeTotal += previousTimes[index];
        if (!frameTimeTotal)
            frameTimeTotal = 1;
        dc->FPS = (float)(4000 / frameTimeTotal);
    }
}
