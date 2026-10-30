#include <universal/q_shared.h>
#include <universal/timing.h>
#include <qcommon/qcommon.h> // SnapFloat

#include "sys_time.h"
#include "sys_posix.h"

#include <time.h>
#include <unistd.h>

long double msecPerRawTimerTick;
double qpc2msec;

// Kept at file scope with the same names as the Windows build so diagnostics
// that reference them keep working.
int initialized_1 = 0;
int sys_timeBase;

uint64_t Posix_RawTimerTicks()
{
    // x86 TSC. The engine only builds for x86 (32-bit), where this is always
    // available; on any other host fall back to the monotonic clock so the
    // platform layer still reports a sane, non-decreasing tick source.
#if defined(__i386__) || defined(__x86_64__)
    return __rdtsc();
#else
    return Posix_MonotonicNanoseconds();
#endif
}

double SecondsPerTick()
{
    // Measure how long a raw timer tick actually lasts by sampling both clocks
    // around a known sleep, mirroring the Windows implementation's intent.
    const uint64_t monoStart = Posix_MonotonicNanoseconds();
    const uint64_t tickStart = Posix_RawTimerTicks();

    usleep(250 * 1000);

    const uint64_t tickStop = Posix_RawTimerTicks();
    const uint64_t monoStop = Posix_MonotonicNanoseconds();

    const uint64_t tickDelta = tickStop - tickStart;
    if (tickDelta == 0)
    {
        // No usable TSC. Report 1:1 against the monotonic clock so callers get
        // a consistent (if uncalibrated) number instead of dividing by zero.
        return 1e-9;
    }

    const double secondsElapsed = (double)(monoStop - monoStart) * 1e-9;
    return secondsElapsed / (double)tickDelta;
}

void InitTiming()
{
    msecPerRawTimerTick = SecondsPerTick() * 1000.0;
    // Retained for source compatibility with callers that convert raw
    // QueryPerformanceCounter units; POSIX uses nanoseconds directly.
    qpc2msec = 1.0 / 1000000.0;

    if (msecPerRawTimerTick <= 0.0)
        msecPerRawTimerTick = 1e-6;
}

uint32_t Sys_Milliseconds()
{
    const uint32_t now = (uint32_t)(Posix_MonotonicNanoseconds() / 1000000ull);

    if (!initialized_1)
    {
        sys_timeBase = (int)now;
        initialized_1 = 1;
    }

    // Deliberately wrapping uint32_t: the engine subtracts previous timestamps
    // and relies on modular arithmetic.
    return now - (uint32_t)sys_timeBase;
}

uint32_t Sys_MillisecondsRaw()
{
    return (uint32_t)(Posix_MonotonicNanoseconds() / 1000000ull);
}

void Sys_SnapVector(float *v)
{
    v[0] = SnapFloat(v[0]);
    v[1] = SnapFloat(v[1]);
    v[2] = SnapFloat(v[2]);
}
