#pragma once

// LONG for the engine's Interlocked* counters, without pulling a Windows header
// into every effects and renderer header. LONG is 32 bits on Windows x86 and x64
// and in the Linux Windows-type shim, unlike long on LP64 Linux.
#ifdef _WIN32
typedef long LONG; // the winnt.h definition; repeating an identical typedef is valid C++
#else
#include <linux/compat.h>
#endif
