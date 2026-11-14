// See vita_call_watch.h.

#include "vita_call_watch.h"

#include <atomic>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>

namespace
{
    using kisak::vita_calls::kPathLength;

    // The main thread's stack: a wrapper knows its thread from the address
    // of one of its own variables, which costs no system call.
    uintptr_t s_stackLow;
    uintptr_t s_stackSize;

    // The call the main thread is inside, for the watcher. The three are not
    // read as one, so a look can pair one call's name with the next call's
    // start: these are for a log line about a call that has run for a
    // quarter of a second.
    std::atomic<const char *> s_inName{nullptr};
    std::atomic<uint64_t> s_inSince{0};
    std::atomic<int32_t> s_inFile{0};
    // The path the call was given, written by the main thread only and read
    // by the watcher as it is; the last byte stays 0.
    char s_inPath[kPathLength];

    // The main thread's calls in this frame.
    struct Frame
    {
        uint64_t time;
        uint32_t calls;
        uint64_t read, written;
        uint64_t longest;
        const char *longestName;
        char longestPath[kPathLength];
    };
    Frame s_frame;

    // The paths of the open files, of every thread, for the calls that take
    // a file and not a path. A file opened when all are taken is not named.
    struct OpenFile
    {
        SceUID file;
        char path[kPathLength];
    };
    constexpr int kOpenFiles = 64;
    pthread_mutex_t s_openLock = PTHREAD_MUTEX_INITIALIZER;
    OpenFile s_open[kOpenFiles];

    uint64_t Now()
    {
        return sceKernelGetProcessTimeWide();
    }

    // The end of a path says more than its start.
    void CopyPath(char *to, const char *from)
    {
        if (!from)
            from = "";
        const size_t length = strlen(from);
        if (length >= kPathLength)
            from += length - (kPathLength - 1);
        strncpy(to, from, kPathLength - 1);
        to[kPathLength - 1] = 0;
    }

    void Opened(SceUID file, const char *path)
    {
        if (file <= 0)
            return;
        pthread_mutex_lock(&s_openLock);
        for (OpenFile &open : s_open)
        {
            if (open.file)
                continue;
            open.file = file;
            CopyPath(open.path, path);
            break;
        }
        pthread_mutex_unlock(&s_openLock);
    }

    void Closed(SceUID file)
    {
        pthread_mutex_lock(&s_openLock);
        for (OpenFile &open : s_open)
        {
            if (open.file == file)
                open.file = 0;
        }
        pthread_mutex_unlock(&s_openLock);
    }

    void PathOf(SceUID file, char *path)
    {
        path[0] = 0;
        if (file <= 0)
            return;
        pthread_mutex_lock(&s_openLock);
        for (const OpenFile &open : s_open)
        {
            if (open.file != file)
                continue;
            memcpy(path, open.path, kPathLength);
            break;
        }
        pthread_mutex_unlock(&s_openLock);
    }

    // One call. On the main thread it says so while the call runs and adds
    // the call to the frame after; on any other thread it does nothing.
    class Call
    {
    public:
        Call(const char *name, SceUID file, const char *path) : m_name(name), m_file(file)
        {
            int here;
            m_main = uintptr_t(&here) - s_stackLow < s_stackSize;
            if (!m_main)
                return;
            if (path)
                CopyPath(s_inPath, path);
            else
                s_inPath[0] = 0;
            m_start = Now();
            s_inFile.store(file, std::memory_order_relaxed);
            s_inSince.store(m_start, std::memory_order_relaxed);
            s_inName.store(name, std::memory_order_release);
        }

        ~Call()
        {
            if (!m_main)
                return;
            s_inName.store(nullptr, std::memory_order_release);
            const uint64_t took = Now() - m_start;
            s_frame.time += took;
            ++s_frame.calls;
            if (took <= s_frame.longest)
                return;
            s_frame.longest = took;
            s_frame.longestName = m_name;
            // Looking the path up takes a lock: only for a call that shows.
            if (s_inPath[0] || took < 1000u)
                memcpy(s_frame.longestPath, s_inPath, kPathLength);
            else
                PathOf(m_file, s_frame.longestPath);
        }

        void Read(int64_t bytes)
        {
            if (m_main && bytes > 0)
                s_frame.read += uint64_t(bytes);
        }

        void Wrote(int64_t bytes)
        {
            if (m_main && bytes > 0)
                s_frame.written += uint64_t(bytes);
        }

    private:
        const char *m_name;
        SceUID m_file;
        bool m_main;
        uint64_t m_start = 0;
    };
}

namespace kisak::vita_calls
{
void SetMainThread()
{
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 || !info.stack)
        return;
    s_stackLow = uintptr_t(info.stack);
    s_stackSize = uintptr_t(info.stackSize);
}

void FrameBegin()
{
    s_frame = Frame();
}

bool Look(Inside &inside, uint64_t now)
{
    const char *name = s_inName.load(std::memory_order_acquire);
    if (!name)
        return false;
    const uint64_t since = s_inSince.load(std::memory_order_relaxed);
    inside.name = name;
    inside.forMs = now > since ? uint32_t((now - since) / 1000u) : 0;
    memcpy(inside.path, s_inPath, kPathLength);
    inside.path[kPathLength - 1] = 0;
    if (!inside.path[0])
        PathOf(s_inFile.load(std::memory_order_relaxed), inside.path);
    return true;
}

void DescribeFrame(char *text, size_t size)
{
    if (!s_frame.calls)
    {
        snprintf(text, size, "no file, memory block or packet call on the main thread");
        return;
    }
    snprintf(text, size,
        "%u file, memory block and packet calls on the main thread took %.1f ms, %.1f KB read and %.1f KB "
        "written; the longest, %s%s%s, took %.1f ms",
        (unsigned int)s_frame.calls, s_frame.time / 1000.0, s_frame.read / 1024.0, s_frame.written / 1024.0,
        s_frame.longestName ? s_frame.longestName : "?", s_frame.longestPath[0] ? " on " : "", s_frame.longestPath,
        s_frame.longest / 1000.0);
}
}

// The wrappers: the linker sends every call of X to __wrap_X and gives the
// system's own X the name __real_X.
extern "C"
{
SceUID __real_sceIoOpen(const char *file, int flags, SceMode mode);
int __real_sceIoClose(SceUID fd);
SceSSize __real_sceIoRead(SceUID fd, void *buf, SceSize nbyte);
SceSSize __real_sceIoWrite(SceUID fd, const void *buf, SceSize nbyte);
int __real_sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset);
int __real_sceIoPwrite(SceUID fd, const void *data, SceSize size, SceOff offset);
SceOff __real_sceIoLseek(SceUID fd, SceOff offset, int whence);
long __real_sceIoLseek32(SceUID fd, long offset, int whence);
int __real_sceIoSyncByFd(SceUID fd, int flag);
int __real_sceIoGetstat(const char *file, SceIoStat *stat);
int __real_sceIoGetstatByFd(SceUID fd, SceIoStat *stat);
SceUID __real_sceIoDopen(const char *dirname);
int __real_sceIoDread(SceUID fd, SceIoDirent *dir);
int __real_sceIoDclose(SceUID fd);
int __real_sceIoRemove(const char *file);
int __real_sceIoRename(const char *oldname, const char *newname);
int __real_sceIoMkdir(const char *dir, SceMode mode);
SceUID __real_sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type, SceSize size,
    SceKernelAllocMemBlockOpt *opt);
int __real_sceKernelFreeMemBlock(SceUID uid);
int __real_sceNetRecvfrom(int s, void *buf, unsigned int len, int flags, SceNetSockaddr *from,
    unsigned int *fromlen);
int __real_sceNetSendto(int s, const void *msg, unsigned int len, int flags, const SceNetSockaddr *to,
    unsigned int tolen);

SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode)
{
    SceUID opened;
    {
        Call call("sceIoOpen", 0, file);
        opened = __real_sceIoOpen(file, flags, mode);
    }
    Opened(opened, file);
    return opened;
}

int __wrap_sceIoClose(SceUID fd)
{
    int result;
    {
        Call call("sceIoClose", fd, nullptr);
        result = __real_sceIoClose(fd);
    }
    Closed(fd);
    return result;
}

SceSSize __wrap_sceIoRead(SceUID fd, void *buf, SceSize nbyte)
{
    Call call("sceIoRead", fd, nullptr);
    const SceSSize result = __real_sceIoRead(fd, buf, nbyte);
    call.Read(result);
    return result;
}

SceSSize __wrap_sceIoWrite(SceUID fd, const void *buf, SceSize nbyte)
{
    Call call("sceIoWrite", fd, nullptr);
    const SceSSize result = __real_sceIoWrite(fd, buf, nbyte);
    call.Wrote(result);
    return result;
}

int __wrap_sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset)
{
    Call call("sceIoPread", fd, nullptr);
    const int result = __real_sceIoPread(fd, data, size, offset);
    call.Read(result);
    return result;
}

int __wrap_sceIoPwrite(SceUID fd, const void *data, SceSize size, SceOff offset)
{
    Call call("sceIoPwrite", fd, nullptr);
    const int result = __real_sceIoPwrite(fd, data, size, offset);
    call.Wrote(result);
    return result;
}

SceOff __wrap_sceIoLseek(SceUID fd, SceOff offset, int whence)
{
    Call call("sceIoLseek", fd, nullptr);
    return __real_sceIoLseek(fd, offset, whence);
}

long __wrap_sceIoLseek32(SceUID fd, long offset, int whence)
{
    Call call("sceIoLseek32", fd, nullptr);
    return __real_sceIoLseek32(fd, offset, whence);
}

int __wrap_sceIoSyncByFd(SceUID fd, int flag)
{
    Call call("sceIoSyncByFd", fd, nullptr);
    return __real_sceIoSyncByFd(fd, flag);
}

int __wrap_sceIoGetstat(const char *file, SceIoStat *stat)
{
    Call call("sceIoGetstat", 0, file);
    return __real_sceIoGetstat(file, stat);
}

int __wrap_sceIoGetstatByFd(SceUID fd, SceIoStat *stat)
{
    Call call("sceIoGetstatByFd", fd, nullptr);
    return __real_sceIoGetstatByFd(fd, stat);
}

SceUID __wrap_sceIoDopen(const char *dirname)
{
    SceUID opened;
    {
        Call call("sceIoDopen", 0, dirname);
        opened = __real_sceIoDopen(dirname);
    }
    Opened(opened, dirname);
    return opened;
}

int __wrap_sceIoDread(SceUID fd, SceIoDirent *dir)
{
    Call call("sceIoDread", fd, nullptr);
    return __real_sceIoDread(fd, dir);
}

int __wrap_sceIoDclose(SceUID fd)
{
    int result;
    {
        Call call("sceIoDclose", fd, nullptr);
        result = __real_sceIoDclose(fd);
    }
    Closed(fd);
    return result;
}

int __wrap_sceIoRemove(const char *file)
{
    Call call("sceIoRemove", 0, file);
    return __real_sceIoRemove(file);
}

int __wrap_sceIoRename(const char *oldname, const char *newname)
{
    Call call("sceIoRename", 0, newname);
    return __real_sceIoRename(oldname, newname);
}

int __wrap_sceIoMkdir(const char *dir, SceMode mode)
{
    Call call("sceIoMkdir", 0, dir);
    return __real_sceIoMkdir(dir, mode);
}

SceUID __wrap_sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type, SceSize size,
    SceKernelAllocMemBlockOpt *opt)
{
    Call call("sceKernelAllocMemBlock", 0, name);
    return __real_sceKernelAllocMemBlock(name, type, size, opt);
}

int __wrap_sceKernelFreeMemBlock(SceUID uid)
{
    Call call("sceKernelFreeMemBlock", 0, nullptr);
    return __real_sceKernelFreeMemBlock(uid);
}

int __wrap_sceNetRecvfrom(int s, void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen)
{
    Call call("sceNetRecvfrom", 0, nullptr);
    return __real_sceNetRecvfrom(s, buf, len, flags, from, fromlen);
}

int __wrap_sceNetSendto(int s, const void *msg, unsigned int len, int flags, const SceNetSockaddr *to,
    unsigned int tolen)
{
    Call call("sceNetSendto", 0, nullptr);
    return __real_sceNetSendto(s, msg, len, flags, to, tolen);
}
}
