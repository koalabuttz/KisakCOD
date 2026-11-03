#pragma once

// Opt-in script string reference trace (configure with -DKISAK_SL_TRACE=ON).
//
// Debug builds count every script string reference and SL_CheckLeaks asserts at
// quit if one is still held. That says which string leaked but not who took the
// reference. With the trace on, every counted reference change records a short
// backtrace in a per-string history, the history is cleared when the string is
// freed, and SL_CheckLeaks prints the history of each leaked string before it
// asserts. Off by default: nothing here is compiled into normal builds.

#include <cstdint>

#if defined(KISAK_SL_TRACE)

enum SlTraceOp : uint8_t
{
    SL_TRACE_NEW = 1,       // string created with its first reference
    SL_TRACE_ADD_REF = 2,   // plain reference added
    SL_TRACE_ADD_USER = 3,  // reference added for a user (system) bit
    SL_TRACE_REMOVE = 4,    // reference removed, string still alive
    SL_TRACE_TRANSFER = 5,  // reference dropped because the user already held one
};

void SL_TraceRecord(uint32_t stringValue, SlTraceOp op, uint32_t user);
void SL_TraceForget(uint32_t stringValue);
void SL_TraceDump(uint32_t stringValue, const char *text, uint32_t user, int32_t debugRefCount);

#define SL_TRACE(stringValue, op, user) SL_TraceRecord((stringValue), (op), (user))
#define SL_TRACE_FORGET(stringValue) SL_TraceForget(stringValue)

#else

#define SL_TRACE(stringValue, op, user) ((void)0)
#define SL_TRACE_FORGET(stringValue) ((void)0)

#endif
