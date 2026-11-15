// The PS Vita build's threads by name, for the CPU time each one used
// (vita_platform.h, Vita_NoteThread and Vita_ThreadTimes).
//
// A user application cannot list its threads, so each thread the port starts
// says its name here once. The kernel counts the time a thread ran
// (SceKernelThreadInfo::runClocks, microseconds), which is what the perf
// windows print as "cpu ms/frame" on the other platforms (sys_perf.cpp).
//
// The same list is how a run moves a thread to other cores or to another
// priority (Vita_SetThreadCores, Vita_SetThreadPriorityStep): the port sets
// neither by itself, and console run BN showed the kernel leaving two busy
// threads to take turns on one core while the other cores had time to spare.

#include "vita_platform.h"

#include <linux/sys_posix.h>

#include <pthread.h>
#include <string.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

namespace
{
    struct Noted
    {
        SceUID id;
        char name[24];
    };
    constexpr int kMaxThreads = 24;
    pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    Noted s_threads[kMaxThreads];
    int s_count;

    // What a run asked for, by a part of a thread's name. It is kept, so a
    // thread that starts later (the submit thread, turned off and on) gets
    // it when it says its name.
    struct Rule
    {
        char part[24];
        bool hasCores, hasStep;
        int cores; // bits 0 to 2: the user cores; 0: all of them
        int step;  // added to the priority the thread started with
    };
    constexpr int kMaxRules = 8;
    Rule s_rules[kMaxRules];
    int s_ruleCount;

    bool NameHasPart(const char *name, const char *part)
    {
        const size_t length = strlen(part);
        for (; *name; ++name)
        {
            if (!strncasecmp(name, part, length))
                return true;
        }
        return length == 0;
    }

    // The kernel's answer: 0, or its error for the first call that failed.
    int Apply(SceUID id, const Rule &rule)
    {
        int result = 0;
        if (rule.hasCores)
        {
            const int mask = rule.cores ? (rule.cores & 7) << 16 : SCE_KERNEL_CPU_MASK_USER_ALL;
            const int changed = sceKernelChangeThreadCpuAffinityMask(id, mask);
            if (changed < 0)
                result = changed;
        }
        if (rule.hasStep)
        {
            SceKernelThreadInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            int changed = sceKernelGetThreadInfo(id, &info);
            if (changed >= 0)
                changed = sceKernelChangeThreadPriority(id, info.initPriority + rule.step);
            if (changed < 0 && !result)
                result = changed;
        }
        return result;
    }

    // With s_lock held.
    Rule *RuleFor(const char *part)
    {
        for (int i = 0; i < s_ruleCount; ++i)
        {
            if (!strcasecmp(s_rules[i].part, part))
                return &s_rules[i];
        }
        if (s_ruleCount >= kMaxRules || strlen(part) >= sizeof(s_rules[0].part))
            return nullptr;
        Rule *rule = &s_rules[s_ruleCount++];
        memset(rule, 0, sizeof(*rule));
        strcpy(rule->part, part);
        return rule;
    }

    // With s_lock held: to every thread of that name that still runs.
    int ApplyToAll(const Rule &rule, int *firstError)
    {
        int applied = 0;
        *firstError = 0;
        for (int i = 0; i < s_count; ++i)
        {
            if (!NameHasPart(s_threads[i].name, rule.part))
                continue;
            SceKernelThreadInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(s_threads[i].id, &info) < 0)
                continue;
            const int result = Apply(s_threads[i].id, rule);
            if (result < 0 && !*firstError)
                *firstError = result;
            ++applied;
        }
        return applied;
    }
}

void Vita_NoteThread(const char *name)
{
    const SceUID self = sceKernelGetThreadId();
    pthread_mutex_lock(&s_lock);
    Noted *noted = nullptr;
    for (int i = 0; i < s_count && !noted; ++i)
    {
        if (s_threads[i].id == self)
            noted = &s_threads[i];
    }
    if (!noted && s_count < kMaxThreads)
        noted = &s_threads[s_count++];
    if (noted)
    {
        noted->id = self;
        strncpy(noted->name, name ? name : "", sizeof(noted->name) - 1);
        noted->name[sizeof(noted->name) - 1] = 0;
        for (int i = 0; i < s_ruleCount; ++i)
        {
            if (NameHasPart(noted->name, s_rules[i].part))
                Apply(self, s_rules[i]);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

int Vita_SetThreadCores(const char *namePart, int cores, int *firstError)
{
    int applied = -1;
    *firstError = 0;
    pthread_mutex_lock(&s_lock);
    if (Rule *rule = RuleFor(namePart))
    {
        rule->hasCores = true;
        rule->cores = cores & 7;
        Rule now = *rule;
        now.hasStep = false;
        applied = ApplyToAll(now, firstError);
    }
    pthread_mutex_unlock(&s_lock);
    return applied;
}

int Vita_SetThreadPriorityStep(const char *namePart, int step, int *firstError)
{
    int applied = -1;
    *firstError = 0;
    pthread_mutex_lock(&s_lock);
    if (Rule *rule = RuleFor(namePart))
    {
        rule->hasStep = true;
        rule->step = step;
        Rule now = *rule;
        now.hasCores = false;
        applied = ApplyToAll(now, firstError);
    }
    pthread_mutex_unlock(&s_lock);
    return applied;
}

int Vita_ThreadRows(VitaThreadRow *rows, int count)
{
    int filled = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_count && filled < count; ++i)
    {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(s_threads[i].id, &info) < 0)
            continue;
        VitaThreadRow &row = rows[filled++];
        row.id = (int)s_threads[i].id;
        memcpy(row.name, s_threads[i].name, sizeof(row.name));
        row.priority = info.currentPriority;
        row.startPriority = info.initPriority;
        row.cores = (info.currentCpuAffinityMask >> 16) & 15;
        row.lastCore = info.lastExecutedCpuId;
        row.preempted = info.threadPreemptCount;
        row.released = info.threadReleaseCount;
        row.movedCore = (unsigned int)info.changeCpuCount;
        row.runMicroseconds = info.runClocks;
    }
    pthread_mutex_unlock(&s_lock);
    return filled;
}

void Vita_QuietWaits()
{
    // pte_cancellable_wait (libpthread.a) takes sceKernelWaitSema with the
    // caller's own timeout when cancellation is off, and a loop of 500 us
    // waits and event flag polls when it is on.
    int before;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &before);
}

uint32_t Vita_TimedWaitMicroseconds(uint32_t msec)
{
    PosixEvent *event = Posix_CreateEvent(true, false);
    const uint64_t start = Posix_MonotonicNanoseconds();
    Posix_WaitEvent(event, msec);
    const uint64_t took = Posix_MonotonicNanoseconds() - start;
    Posix_DestroyEvent(event);
    return (uint32_t)(took / 1000u);
}

int Vita_ThreadTimes(VitaThreadTime *times, int count)
{
    int filled = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_count && filled < count; ++i)
    {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        // A thread that has ended is no longer there to ask.
        if (sceKernelGetThreadInfo(s_threads[i].id, &info) < 0)
            continue;
        VitaThreadTime &time = times[filled++];
        time.id = (int)s_threads[i].id;
        memcpy(time.name, s_threads[i].name, sizeof(time.name));
        time.runMicroseconds = info.runClocks;
        time.self = s_threads[i].id == sceKernelGetThreadId();
    }
    pthread_mutex_unlock(&s_lock);
    return filled;
}
