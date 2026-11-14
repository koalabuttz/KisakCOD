// The PS Vita build's threads by name, for the CPU time each one used
// (vita_platform.h, Vita_NoteThread and Vita_ThreadTimes).
//
// A user application cannot list its threads, so each thread the port starts
// says its name here once. The kernel counts the time a thread ran
// (SceKernelThreadInfo::runClocks, microseconds), which is what the perf
// windows print as "cpu ms/frame" on the other platforms (sys_perf.cpp).

#include "vita_platform.h"

#include <pthread.h>
#include <string.h>

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
    }
    pthread_mutex_unlock(&s_lock);
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
