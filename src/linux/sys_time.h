#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Native POSIX timing services.
//
// These replace universal/timing.cpp and the Sys_Milliseconds/Sys_SnapVector
// half of universal/win_shared.cpp, both of which are left out of the native
// Linux target. The exported names and their signatures are unchanged: engine
// code includes <universal/q_shared.h> and <universal/timing.h> and calls these
// exactly as before.
//
// Sys_Milliseconds     milliseconds since first call (wrapping uint32_t, as the
//                      engine's frame delta logic expects)
// Sys_MillisecondsRaw  milliseconds since an arbitrary fixed origin; never
//                      re-based, so it is safe to compare across the process
// msecPerRawTimerTick  calibrated milliseconds per raw timer (TSC) tick
// ---------------------------------------------------------------------------

// Slow, calibration-only measurement of the raw timer frequency.
double SecondsPerTick();

// Calibrate msecPerRawTimerTick. Must run before Sys_FindInfo/Sys_Milliseconds.
void InitTiming();

extern long double msecPerRawTimerTick;
extern double qpc2msec;

// Nanoseconds from CLOCK_MONOTONIC. Monotonic and unaffected by wall-clock
// steps; used by the platform layer and available to other Linux services.
uint64_t Posix_MonotonicNanoseconds();

// Raw timer (TSC) reading, in ticks.
uint64_t Posix_RawTimerTicks();
