// Load and memory figures for the PS Vita dedicated server's first runs. See
// vita_platform.h and docs/vita.md ("Reading console.log").
//
// The Vita's CPU sets the player cap, so the log has to say how much of each
// second the server spends working. The main loop wraps each Com_Frame
// (Vita_FrameBegin/End) and NET_Sleep reports the time it waited for packets;
// the rest is work: packet handling, game frames, snapshots, bots.
//
// It also times the runs nobody watches (vita_run_plan.h): the period of the
// figures, the quit timers counted from start-up, from the menu or from the
// map, and the commands sent after the menu or the map came up.

#include "vita_platform.h"
#include "vita_mman.h"
#include "vita_run_plan.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <universal/physicalmemory.h>

#if defined(KISAK_VITA_MP_CLIENT)
#include <client/client.h>
#include <client_mp/client_mp.h>
#include <ui_mp/ui_mp.h>
#endif

#include <atomic>

#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

namespace
{
    // One summary line a minute by default (vita_statsEvery): often enough to
    // see a map load or bots joining, rare enough to keep console.log short
    // over a long session.
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

    // Long frames. Console runs on 2026-11-14 (AQ to AU) found one or two
    // frames a start in which the main thread did not come back for 0.3 to
    // 0.9 s while nothing was loaded or made and no timed part of the frame
    // ran long. A watcher thread looks at the main thread while a frame runs
    // long: whether it is running, ready (another thread has its core) or
    // waiting and on what, and how much it ran. Vita_FrameEnd logs what it
    // saw (WatchFrames, LogLongFrame).
    constexpr uint64_t kLongFrame = 250u * 1000u;
    constexpr uint64_t kLookEvery = 100u * 1000u;
    constexpr int kMaxLooks = 8;
    struct Look
    {
        uint32_t at;     // ms into the frame
        uint32_t status; // SceThreadStatus
        uint32_t waitType;
        SceUID waitId;
        uint64_t runClocks; // the main thread's run time so far, us
        uint32_t threadPreempts, intrPreempts;
        int32_t cpu, priority;
    };
    SceUID s_mainThread = -1;
    // The frame being watched (its start, 0 between frames) and the looks
    // at it, under s_watchLock: a look taken as the frame ends is dropped.
    pthread_mutex_t s_watchLock = PTHREAD_MUTEX_INITIALIZER;
    std::atomic<uint64_t> s_watchedStart{0};
    int s_lookCount;
    Look s_looks[kMaxLooks];

    cmd_function_s s_statsCommand;

    // Runs nobody watches (vita_run_plan.h).
    const dvar_t *vita_quitAfter;
    const dvar_t *vita_quitAfterMenu;
    const dvar_t *vita_quitAfterMap;
    const dvar_t *vita_statsEvery;
    const dvar_t *vita_atMenu;
    const dvar_t *vita_atMap;
    const dvar_t *vita_assertExit;
    // vita_atMenu and vita_atMap as the start gave them: their commands go to
    // the console, so the dvars are read once (kisak::vita_run::StartText).
    kisak::vita_run::StartText<kisak::vita_run::kMaxTimedListLength> s_atMenu;
    kisak::vita_run::StartText<kisak::vita_run::kMaxTimedListLength> s_atMap;
    // What vita_atMenu and vita_atMap send: their text, or the steps of the
    // file they name ("@<file>"), read with them at start-up.
    char s_atMenuSteps[kisak::vita_run::kMaxTimedListLength];
    char s_atMapSteps[kisak::vita_run::kMaxTimedListLength];
    const char *s_atMenuUsed = "";
    const char *s_atMapUsed = "";
    kisak::vita_run::Plan s_plan;
    bool s_quitDue;

    // Counts the times every figure was asked for. The renderer and the
    // sound thread print theirs when they see it change.
    std::atomic<unsigned int> s_statsGeneration;

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
        s_statsGeneration.fetch_add(1, std::memory_order_relaxed);
    }

    // The load since the last line, memory, and a nudge for the renderer's
    // and the sound's own lines.
    void ReportAll(uint64_t now)
    {
        PrintLoad(s_window, now);
        PrintMemory("now");
        s_window = Window();
        s_window.start = now;
        s_statsGeneration.fetch_add(1, std::memory_order_relaxed);
    }

    // What the run plan waits for. The server alone has no menu; its map is
    // "up" once it runs one.
    bool MenuIsUp()
    {
#if defined(KISAK_VITA_MP_CLIENT)
        return cls.uiStarted && CL_GetLocalClientConnectionState(0) == CA_DISCONNECTED && UI_IsFullscreen(0);
#else
        return false;
#endif
    }

    bool MapIsUp()
    {
#if defined(KISAK_VITA_MP_CLIENT)
        return CL_GetLocalClientConnectionState(0) == CA_ACTIVE;
#else
        return com_sv_running && com_sv_running->current.enabled;
#endif
    }

    // The script menus the server named, by number: what "openscriptmenu
    // <name> <response>" and "cmd mr {serverid} <number> <response>" address,
    // so a run can pick a team and a class without anyone at the screen.
    void PrintScriptMenus()
    {
#if defined(KISAK_VITA_MP_CLIENT)
        char line[1024];
        size_t used = 0;
        line[0] = '\0';
        for (int index = 0; index < CS_SCRIPT_MENUS_COUNT; ++index)
        {
            const char *const name = CL_GetConfigString(0, index + CS_SCRIPT_MENUS);
            if (!name || !*name)
                continue;
            const int wrote = snprintf(line + used, sizeof(line) - used, "%s%d %s", used ? ", " : "", index, name);
            if (wrote < 0 || size_t(wrote) >= sizeof(line) - used)
            {
                line[used] = '\0';
                break;
            }
            used += size_t(wrote);
        }
        Com_Printf(CON_CHANNEL_SYSTEM, "Vita: script menus: %s\n", used ? line : "none");
#endif
    }

    // A timed command goes to the console as it is, with "{serverid}"
    // replaced by the server id a menu response has to carry.
    void SendCommand(const char *command)
    {
        char text[kisak::vita_run::kMaxCommandLength + 32];
        size_t used = 0;
        static const char kToken[] = "{serverid}";
        for (const char *from = command; *from && used + 16 < sizeof(text);)
        {
            if (!strncmp(from, kToken, sizeof(kToken) - 1))
            {
                used += size_t(snprintf(text + used, sizeof(text) - used, "%d", Dvar_GetInt("sv_serverId")));
                from += sizeof(kToken) - 1;
            }
            else
                text[used++] = *from++;
        }
        text[used] = '\0';
        Com_Printf(CON_CHANNEL_SYSTEM, "Vita: sending \"%s\"\n", text);
        Cbuf_AddText(0, text);
        Cbuf_AddText(0, "\n");
    }

    // vita_atMenu or vita_atMap as taken at start-up: its text, or with
    // "@<file>" the steps in that file of the data folder (one
    // "<seconds> <command>" a line). What cannot be read sends nothing.
    const char *StepsOf(const char *dvarName, const char *taken, char *steps, size_t stepsSize)
    {
        if (taken[0] != '@')
        {
            if (taken[0])
                Com_Printf(CON_CHANNEL_SYSTEM, "Vita: %s \"%s\"\n", dvarName, taken);
            return taken;
        }
        char name[kisak::vita_run::kMaxStepFileName];
        if (!kisak::vita_run::StepFileName(taken, name, sizeof(name)))
        {
            Com_PrintError(CON_CHANNEL_SYSTEM, "Vita: %s \"%s\" does not name a plain file in %s; none of its "
                "commands will be sent\n", dvarName, taken, Sys_VitaDataPath());
            return "";
        }
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", Sys_VitaDataPath(), name);
        // Room for the steps and a comment line or two each.
        static char fileText[3 * kisak::vita_run::kMaxTimedListLength];
        FILE *file = fopen(path, "rb");
        size_t length = 0;
        bool whole = false;
        if (file)
        {
            length = fread(fileText, 1, sizeof(fileText) - 1, file);
            whole = length < sizeof(fileText) - 1 || fgetc(file) == EOF;
            fclose(file);
        }
        fileText[length] = '\0';
        if (!file || !whole)
        {
            Com_PrintError(CON_CHANNEL_SYSTEM, "Vita: %s: %s %s; none of its commands will be sent\n", dvarName, path,
                file ? "is too big" : "cannot be opened");
            return "";
        }
        char error[96];
        const int count = kisak::vita_run::JoinStepFile(fileText, steps, stepsSize, error, sizeof(error));
        if (count < 0)
        {
            Com_PrintError(CON_CHANNEL_SYSTEM, "Vita: %s: %s %s; none of its commands will be sent\n", dvarName, path,
                error);
            return "";
        }
        Com_Printf(CON_CHANNEL_SYSTEM, "Vita: %s from %s: %d steps\n", dvarName, path, count);
        return steps;
    }

    unsigned int Seconds(const dvar_t *dvar)
    {
        return dvar && dvar->current.integer > 0 ? (unsigned int)dvar->current.integer : 0;
    }

    void RunPlan(uint64_t now)
    {
        kisak::vita_run::Settings settings;
        settings.quitAfter = Seconds(vita_quitAfter);
        settings.quitAfterMenu = Seconds(vita_quitAfterMenu);
        settings.quitAfterMap = Seconds(vita_quitAfterMap);
        settings.statsEvery = Seconds(vita_statsEvery);
        settings.atMenu = s_atMenuUsed;
        settings.atMap = s_atMapUsed;
        const kisak::vita_run::Step step = s_plan.Frame(now, MenuIsUp(), MapIsUp(), settings);

        if (step.menuCameUp)
            Com_Printf(CON_CHANNEL_SYSTEM, "Vita: the menu is up\n");
        if (step.mapCameUp)
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "Vita: the map is up\n");
            PrintScriptMenus();
        }
        if (step.commandError[0])
            Com_PrintError(CON_CHANNEL_SYSTEM, "Vita: %s; none of its commands will be sent\n", step.commandError);
        for (size_t i = 0; i < step.commandCount; ++i)
            SendCommand(step.commands[i]);
        if (step.report)
        {
            if (step.quitDvar)
                Com_Printf(CON_CHANNEL_SYSTEM, "Vita: %s %u reached, last figures:\n", step.quitDvar, step.quitSeconds);
            ReportAll(now);
        }
        if (step.quit)
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "Vita: quitting as asked\n");
            s_quitDue = true;
        }
    }
}

namespace
{
    bool LookAtMain(Look &look, uint64_t age)
    {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(s_mainThread, &info) < 0)
            return false;
        look.at = (uint32_t)(age / 1000u);
        look.status = info.status;
        look.waitType = info.waitType;
        look.waitId = info.waitId;
        look.runClocks = info.runClocks;
        look.threadPreempts = info.threadPreemptCount;
        look.intrPreempts = info.intrPreemptCount;
        look.cpu = info.currentCpuId;
        look.priority = info.currentPriority;
        return true;
    }

    int WatchFrames(SceSize, void *)
    {
        uint64_t watched = 0;
        uint64_t nextLook = 0;
        for (;;)
        {
            sceKernelDelayThread(20 * 1000);
            const uint64_t start = s_watchedStart.load(std::memory_order_acquire);
            if (!start)
                continue;
            if (start != watched)
            {
                watched = start;
                nextLook = kLongFrame;
            }
            const uint64_t age = Now() - start;
            if (age < nextLook)
                continue;
            nextLook += kLookEvery;
            Look look;
            if (!LookAtMain(look, age))
                continue;
            pthread_mutex_lock(&s_watchLock);
            if (s_watchedStart.load(std::memory_order_relaxed) == start && s_lookCount < kMaxLooks)
                s_looks[s_lookCount++] = look;
            pthread_mutex_unlock(&s_watchLock);
        }
        return 0;
    }

    const char *StatusName(uint32_t status)
    {
        switch (status)
        {
        case SCE_THREAD_RUNNING: return "running";
        case SCE_THREAD_READY: return "ready, not running";
        case SCE_THREAD_WAITING: return "waiting";
        case SCE_THREAD_SUSPENDED: return "suspended";
        default: return "other";
        }
    }

    void LogLongFrame(uint64_t length, int count)
    {
        // Each look and what the main thread did since the one before.
        char line[1024];
        int used = snprintf(line, sizeof(line), "Vita: long frame, %.0f ms; the main thread", length / 1000.0);
        for (int i = 0; i < count && used > 0 && used < (int)sizeof(line); ++i)
        {
            const Look &look = s_looks[i];
            int more;
            if (i == 0)
                more = snprintf(line + used, sizeof(line) - used, " at %u ms: %s", look.at, StatusName(look.status));
            else
            {
                const Look &before = s_looks[i - 1];
                more = snprintf(line + used, sizeof(line) - used,
                    "; at %u ms: %s, ran %u ms of %u, %u thread and %u interrupt preemptions", look.at, StatusName(look.status), (unsigned int)((look.runClocks - before.runClocks) / 1000u),
                    look.at - before.at, look.threadPreempts - before.threadPreempts,
                    look.intrPreempts - before.intrPreempts);
            }
            if (more > 0 && used + more < (int)sizeof(line) && look.status == SCE_THREAD_WAITING)
            {
                used += more;
                more = snprintf(line + used, sizeof(line) - used, " (wait type 0x%x on 0x%x)",
                    (unsigned int)look.waitType, (unsigned int)look.waitId);
            }
            if (more > 0 && used + more < (int)sizeof(line))
            {
                used += more;
                more = snprintf(line + used, sizeof(line) - used, ", core %d, priority 0x%x", (int)look.cpu,
                    (unsigned int)look.priority);
            }
            if (more <= 0 || used + more >= (int)sizeof(line))
                break;
            used += more;
        }
        Com_Printf(CON_CHANNEL_SYSTEM, "%s\n", line);
    }
}

void Vita_StatsInit()
{
    Cmd_AddCommandInternal("vitastats", Stats_f, &s_statsCommand);
    vita_quitAfter = Dvar_RegisterInt("vita_quitAfter", 0, 0, 86400, DVAR_NOFLAG,
        "Quit this many seconds after start-up, for runs nobody watches (0: never)");
    vita_quitAfterMenu = Dvar_RegisterInt("vita_quitAfterMenu", 0, 0, 86400, DVAR_NOFLAG,
        "Quit this many seconds after the main menu first showed (0: never)");
    vita_quitAfterMap = Dvar_RegisterInt("vita_quitAfterMap", 0, 0, 86400, DVAR_NOFLAG,
        "Quit this many seconds after the map was first in play (0: never)");
    vita_statsEvery = Dvar_RegisterInt("vita_statsEvery", 60, 1, 86400, DVAR_NOFLAG,
        "Seconds between the load, memory and sound lines in the log");
    vita_atMenu = Dvar_RegisterString("vita_atMenu", "", DVAR_NOFLAG,
        "Console commands to send after the main menu first showed: \"<seconds> <command> | <seconds> <command>\", "
        "or \"@<file>\" for a file of such steps in the data folder, one a line (read at start-up only)");
    vita_atMap = Dvar_RegisterString("vita_atMap", "", DVAR_NOFLAG,
        "Console commands to send after the map was first in play: \"<seconds> <command> | <seconds> <command>\"; "
        "or \"@<file>\" for a file of such steps in the data folder, one a line; {serverid} stands for the server id "
        "(read at start-up only)");
    // Read here and nowhere else: a game server can set either dvar later, and
    // what they hold is run as console commands.
    if (!s_atMenu.Take(vita_atMenu ? vita_atMenu->current.string : ""))
        Com_PrintError(CON_CHANNEL_SYSTEM, "Vita: vita_atMenu is longer than %u characters; none of its commands will be sent\n",
            (unsigned int)(kisak::vita_run::kMaxTimedListLength - 1));
    if (!s_atMap.Take(vita_atMap ? vita_atMap->current.string : ""))
        Com_PrintError(CON_CHANNEL_SYSTEM, "Vita: vita_atMap is longer than %u characters; none of its commands will be sent\n",
            (unsigned int)(kisak::vita_run::kMaxTimedListLength - 1));
    vita_assertExit = Dvar_RegisterBool("vita_assertExit", Vita_AssertExits(), DVAR_NOFLAG,
        "A failed assertion or an uncaught exception ends the process with exit code 1 and no core dump "
        "(for emulator runs)");
    // The log does not hold the command line (it may carry a key), so a run's
    // report could not show what the start was asked to do.
    Com_Printf(CON_CHANNEL_SYSTEM,
        "Vita: run switches: vita_quitAfter %u, vita_quitAfterMenu %u, vita_quitAfterMap %u, vita_statsEvery %u, "
        "vita_assertExit %d\n",
        Seconds(vita_quitAfter), Seconds(vita_quitAfterMenu), Seconds(vita_quitAfterMap), Seconds(vita_statsEvery),
        vita_assertExit && vita_assertExit->current.enabled ? 1 : 0);
    s_atMenuUsed = StepsOf("vita_atMenu", s_atMenu.Get(), s_atMenuSteps, sizeof(s_atMenuSteps));
    s_atMapUsed = StepsOf("vita_atMap", s_atMap.Get(), s_atMapSteps, sizeof(s_atMapSteps));
    PrintMemory("after start-up");
    s_mainThread = sceKernelGetThreadId();
    // High priority so that it sees the main thread even when the main
    // thread's core is taken; it sleeps all but a moment of every 20 ms.
    const SceUID watcher = sceKernelCreateThread("kisak_frame_watch", WatchFrames, 0x40, 16 * 1024, 0, 0, nullptr);
    if (watcher < 0 || sceKernelStartThread(watcher, 0, nullptr) < 0)
        Com_Printf(CON_CHANNEL_SYSTEM, "Vita: no long-frame watcher (0x%08x)\n", (unsigned int)watcher);
    s_window = Window();
    s_window.start = Now();
    s_plan.Start(0, s_window.start);
    s_started = true;
}

void Vita_FrameBegin()
{
    s_frameStart = Now();
    s_frameIdle = 0;
    s_watchedStart.store(s_frameStart, std::memory_order_release);
}

void Vita_FrameEnd()
{
    if (!s_started)
        return;
    const uint64_t now = Now();
    const uint64_t length = now - s_frameStart;
    const uint64_t work = length > s_frameIdle ? length - s_frameIdle : 0;
    pthread_mutex_lock(&s_watchLock);
    s_watchedStart.store(0, std::memory_order_relaxed);
    const int looks = s_lookCount;
    s_lookCount = 0;
    pthread_mutex_unlock(&s_watchLock);
    if (looks)
        LogLongFrame(length, looks);
    s_window.idle += s_frameIdle;
    if (work > s_window.slowestFrame)
        s_window.slowestFrame = work;
    if (work > kFrameBudget)
        ++s_window.framesOverBudget;

    if (vita_assertExit)
        Vita_SetAssertExits(vita_assertExit->current.enabled);
    RunPlan(now);
}

bool Vita_QuitAfterDue()
{
    // Once: quitting takes a few frames.
    const bool due = s_quitDue;
    s_quitDue = false;
    return due;
}

unsigned int Vita_StatsGeneration()
{
    return s_statsGeneration.load(std::memory_order_relaxed);
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
    // An emulator echoes this; it survives when the file's tail does not.
    fputs(line, stdout);
    fflush(stdout);
}
