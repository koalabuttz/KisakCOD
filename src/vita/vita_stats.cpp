// Load and memory figures for the PS Vita dedicated server's first runs. See
// vita_platform.h and docs/vita.md ("Reading console.log").
//
// The Vita's CPU sets the player cap, so the log has to say how much of each
// second the server spends working. The main loop wraps each Com_Frame
// (Vita_FrameBegin/End) and NET_Sleep reports the time it waited for packets;
// the rest is work: packet handling, game frames, snapshots, bots.

#include "vita_platform.h"
#include "vita_mman.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <universal/physicalmemory.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

namespace
{
    // One summary line a minute: often enough to see a map load or bots
    // joining, rare enough to keep console.log short over a long session.
    constexpr uint64_t kReportInterval = 60u * 1000000u;
    // One server frame at the default sv_fps 20. A Com_Frame that works
    // longer than this (it runs at most one or two server frames) makes the
    // server fall behind.
    constexpr uint64_t kFrameBudget = 50u * 1000u;

    struct Window
    {
        uint64_t start = 0;
        uint64_t idle = 0;
        uint64_t slowestFrame = 0;
        unsigned int framesOverBudget = 0;
    };

    bool s_started;
    Window s_window;
    uint64_t s_frameStart;
    uint64_t s_frameIdle;

    cmd_function_s s_statsCommand;

    uint64_t Now()
    {
        return sceKernelGetProcessTimeWide();
    }

    double Megabytes(uint64_t bytes)
    {
        return (double)bytes / (1024.0 * 1024.0);
    }

    // Game memory: what is left in the engine's 128 MB arena for zones.
    // Heap: newlib's malloc, out of its fixed block (_newlib_heap_size_user).
    // System: what the Vita can still hand out, after all of the above.
    void FormatMemory(char *line, size_t size, const char *when)
    {
        SceKernelFreeMemorySizeInfo system;
        system.size = sizeof(system);
        if (sceKernelGetFreeMemorySize(&system) < 0)
            system.size_user = system.size_cdram = system.size_phycont = 0;
        const struct mallinfo heap = mallinfo();
        snprintf(line, size,
            "Vita memory %s: game memory %.1f MB free, heap %.1f of %d MB used, "
            "engine reservations %.1f MB in %u blocks, system %.1f MB free\n",
            when, Megabytes(PMem_GetFreeAmount()), Megabytes(heap.uordblks),
            Vita_HeapSizeMB(), Megabytes(Vita_MmapBytes()), Vita_MmapCount(),
            Megabytes((uint32_t)system.size_user));
    }

    void PrintMemory(const char *when)
    {
        char line[256];
        FormatMemory(line, sizeof(line), when);
        Com_Printf(CON_CHANNEL_SYSTEM, "%s", line);
    }

    void PrintLoad(const Window &window, uint64_t now)
    {
        const uint64_t elapsed = now - window.start;
        if (elapsed == 0)
            return;
        const uint64_t busy = elapsed > window.idle ? elapsed - window.idle : 0;
        Com_Printf(CON_CHANNEL_SYSTEM,
            "Vita load, last %.0f s: busy %.1f%%, slowest frame %.1f ms, %u frames over %.0f ms\n",
            (double)elapsed / 1e6, 100.0 * (double)busy / (double)elapsed,
            (double)window.slowestFrame / 1000.0, window.framesOverBudget,
            (double)kFrameBudget / 1000.0);
    }

    void Stats_f()
    {
        if (s_started)
            PrintLoad(s_window, Now());
        PrintMemory("now");
    }
}

void Vita_StatsInit()
{
    Cmd_AddCommandInternal("vitastats", Stats_f, &s_statsCommand);
    PrintMemory("after start-up");
    s_window = Window();
    s_window.start = Now();
    s_started = true;
}

void Vita_FrameBegin()
{
    s_frameStart = Now();
    s_frameIdle = 0;
}

void Vita_FrameEnd()
{
    if (!s_started)
        return;
    const uint64_t now = Now();
    const uint64_t length = now - s_frameStart;
    const uint64_t work = length > s_frameIdle ? length - s_frameIdle : 0;
    s_window.idle += s_frameIdle;
    if (work > s_window.slowestFrame)
        s_window.slowestFrame = work;
    if (work > kFrameBudget)
        ++s_window.framesOverBudget;

    if (now - s_window.start >= kReportInterval)
    {
        PrintLoad(s_window, now);
        PrintMemory("now");
        s_window = Window();
        s_window.start = now;
    }
}

void Vita_AddIdleTime(uint64_t microseconds)
{
    // Com_Frame waits for packets on the main thread. The dvar code's lock
    // spins call NET_Sleep from other threads too; their waits are not the
    // server idling.
    if (Sys_IsMainThread())
        s_frameIdle += microseconds;
}

void Vita_LogMemoryAtError()
{
    // Straight to console.log: the error path holds the engine's locks.
    char line[256];
    FormatMemory(line, sizeof(line), "at the error");
    Vita_ConsoleWrite(line, strlen(line));
}
