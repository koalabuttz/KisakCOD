// Asset-free test of the opt-in script string reference trace
// (src/linux/sl_trace.cpp, built with KISAK_SL_TRACE). The Python driver checks
// what SL_TraceDump prints to stderr.

#include <linux/sl_trace.h>

#include <cstdio>

extern "C" __attribute__((noinline)) void SlTraceTestTakeReference(uint32_t stringValue)
{
    SL_TRACE(stringValue, SL_TRACE_ADD_REF, 0);
    __asm__ volatile("" ::: "memory");
}

int main()
{
    // String 7: a full lifetime, then forgotten when it is freed.
    SL_TRACE(7, SL_TRACE_NEW, 0);
    SlTraceTestTakeReference(7);
    SL_TRACE(7, SL_TRACE_REMOVE, 0);
    SL_TRACE_FORGET(7);

    // The slot is reused: only these events belong to the new string.
    SL_TRACE(7, SL_TRACE_NEW, 4);
    SL_TRACE(7, SL_TRACE_ADD_USER, 4);
    SlTraceTestTakeReference(7);
    SL_TraceDump(7, "slot_seven", 0, 1);

    // String 9: more events than the history holds.
    for (int i = 0; i < 20; ++i)
        SL_TRACE(9, SL_TRACE_ADD_REF, 0);
    SL_TraceDump(9, "slot_nine", 0, 20);

    // Never recorded, and out of range.
    SL_TraceDump(11, "slot_eleven", 0, 1);
    SL_TRACE(70000, SL_TRACE_ADD_REF, 0);
    SL_TRACE_FORGET(70000);
    SL_TraceDump(70000, "out_of_range", 0, 1);

    printf("sl_trace_test: done\n");
    return 0;
}
