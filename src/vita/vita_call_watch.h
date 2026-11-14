// System calls the PS Vita's main thread makes, timed, so that a long frame
// can say which call it sat in (vita_stats.cpp, "Vita: long frame").
//
// A look at the main thread from another thread tells whether it runs or
// waits, not where: console run AV (2026-11-14) found it waiting on a kernel
// event flag half of a 0.76 s frame and running the rest, which is what a
// file read looks like and also what several other calls look like. The
// link wraps the file calls, the memory block calls and the two packet
// calls (-Wl,--wrap in scripts/platform/vita/CMakeLists.txt; the wrappers
// are in vita_call_watch.cpp), which covers newlib's own use of them. A
// wrapper on another thread does nothing but compare a stack address.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kisak::vita_calls
{
constexpr size_t kPathLength = 56;

// The calling thread is the main thread: its calls are the ones timed.
void SetMainThread();

// The main thread's frame starts: its totals start at nothing.
void FrameBegin();

// For another thread: the call the main thread is inside at `now`
// (sceKernelGetProcessTimeWide). False when it is in none of them.
struct Inside
{
    const char *name;
    uint32_t forMs;
    char path[kPathLength];
};
bool Look(Inside &inside, uint64_t now);

// For the main thread: its calls since FrameBegin, as text for the log.
void DescribeFrame(char *text, size_t size);
}
