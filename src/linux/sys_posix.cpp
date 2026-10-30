#include "sys_posix.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Monotonic clock
// ---------------------------------------------------------------------------

uint64_t Posix_MonotonicNanoseconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

struct PosixEvent
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool manualReset;
    bool signalled;
};

PosixEvent *Posix_CreateEvent(bool manualReset, bool initialState)
{
    PosixEvent *event = new PosixEvent();
    if (!event)
        return nullptr;

    pthread_mutex_init(&event->mutex, nullptr);

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    // Monotonic so a timed wait cannot be skewed by a wall-clock step.
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&event->cond, &attr);
    pthread_condattr_destroy(&attr);

    event->manualReset = manualReset;
    event->signalled = initialState;
    return event;
}

void Posix_SetEvent(PosixEvent *event)
{
    if (!event)
        return;

    pthread_mutex_lock(&event->mutex);
    event->signalled = true;
    if (event->manualReset)
        pthread_cond_broadcast(&event->cond);
    else
        pthread_cond_signal(&event->cond);
    pthread_mutex_unlock(&event->mutex);
}

void Posix_ResetEvent(PosixEvent *event)
{
    if (!event)
        return;

    pthread_mutex_lock(&event->mutex);
    event->signalled = false;
    pthread_mutex_unlock(&event->mutex);
}

void Posix_DestroyEvent(PosixEvent *event)
{
    if (!event)
        return;

    pthread_mutex_destroy(&event->mutex);
    pthread_cond_destroy(&event->cond);
    delete event;
}

bool Posix_WaitEvent(PosixEvent *event, uint32_t msec)
{
    if (!event)
        return false;

    bool result = false;
    pthread_mutex_lock(&event->mutex);

    // Predicate loop: a spurious wakeup is not a signal.
    if (!event->signalled)
    {
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(msec / 1000u);
        deadline.tv_nsec += (long)(msec % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        while (!event->signalled)
        {
            const int status = pthread_cond_timedwait(&event->cond, &event->mutex, &deadline);
            if (status == ETIMEDOUT)
                break;
        }
    }

    if (event->signalled)
    {
        result = true;
        if (!event->manualReset)
            event->signalled = false; // consume so exactly one waiter proceeds
    }

    pthread_mutex_unlock(&event->mutex);
    return result;
}

void Posix_WaitEventInfinite(PosixEvent *event)
{
    if (!event)
        return;

    pthread_mutex_lock(&event->mutex);
    while (!event->signalled)
        pthread_cond_wait(&event->cond, &event->mutex);

    if (!event->manualReset)
        event->signalled = false;
    pthread_mutex_unlock(&event->mutex);
}

// ---------------------------------------------------------------------------
// Cooperative suspension
// ---------------------------------------------------------------------------

namespace
{
    struct SuspendGate
    {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        // Nesting depth of outstanding requests; the thread runs only at zero.
        int suspendCount;
        // True while a thread is blocked in the checkpoint on this gate.
        bool parked;
    };

    constexpr int kMaxSuspendGates = 16;
    SuspendGate s_gates[kMaxSuspendGates];
    int s_gateCount = 0;
}

void Posix_InitSuspendGates(int count)
{
    if (count > kMaxSuspendGates)
        count = kMaxSuspendGates;

    // Only build synchronisation objects for gates that do not have them yet, so
    // repeated initialisation cannot re-use a live mutex. The state of an
    // already-initialised gate is reset instead.
    for (int i = 0; i < count; ++i)
    {
        if (i >= s_gateCount)
        {
            pthread_mutex_init(&s_gates[i].mutex, nullptr);

            pthread_condattr_t attr;
            pthread_condattr_init(&attr);
            pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
            pthread_cond_init(&s_gates[i].cond, &attr);
            pthread_condattr_destroy(&attr);
        }

        pthread_mutex_lock(&s_gates[i].mutex);
        s_gates[i].suspendCount = 0;
        s_gates[i].parked = false;
        pthread_cond_broadcast(&s_gates[i].cond);
        pthread_mutex_unlock(&s_gates[i].mutex);
    }

    if (count > s_gateCount)
        s_gateCount = count;
}

static bool GateIndexValid(int index)
{
    return index >= 0 && index < s_gateCount;
}

void Posix_RequestSuspend(int index)
{
    if (!GateIndexValid(index))
        return;

    pthread_mutex_lock(&s_gates[index].mutex);
    if (s_gates[index].suspendCount < INT_MAX)
        ++s_gates[index].suspendCount;
    pthread_mutex_unlock(&s_gates[index].mutex);
}

void Posix_RequestResume(int index)
{
    if (!GateIndexValid(index))
        return;

    pthread_mutex_lock(&s_gates[index].mutex);
    if (s_gates[index].suspendCount > 0)
        --s_gates[index].suspendCount;
    if (s_gates[index].suspendCount == 0)
        pthread_cond_broadcast(&s_gates[index].cond);
    pthread_mutex_unlock(&s_gates[index].mutex);
}

int Posix_SuspendCount(int index)
{
    if (!GateIndexValid(index))
        return 0;

    pthread_mutex_lock(&s_gates[index].mutex);
    const int count = s_gates[index].suspendCount;
    pthread_mutex_unlock(&s_gates[index].mutex);
    return count;
}

bool Posix_IsSuspendRequested(int index)
{
    if (!GateIndexValid(index))
        return false;

    pthread_mutex_lock(&s_gates[index].mutex);
    const bool requested = s_gates[index].suspendCount > 0;
    pthread_mutex_unlock(&s_gates[index].mutex);
    return requested;
}

void Posix_SuspendCheckpoint(int index)
{
    if (!GateIndexValid(index))
        return;

    pthread_mutex_lock(&s_gates[index].mutex);

    if (s_gates[index].suspendCount > 0)
    {
        // Publish that this thread has actually stopped, so a caller waiting for
        // quiescence can proceed, then block until every request is withdrawn.
        s_gates[index].parked = true;
        pthread_cond_broadcast(&s_gates[index].cond);

        while (s_gates[index].suspendCount > 0)
            pthread_cond_wait(&s_gates[index].cond, &s_gates[index].mutex);

        s_gates[index].parked = false;
    }

    pthread_mutex_unlock(&s_gates[index].mutex);
}

bool Posix_WaitForSuspendAck(int index, uint32_t msec)
{
    if (!GateIndexValid(index))
        return false;

    bool acknowledged = false;
    pthread_mutex_lock(&s_gates[index].mutex);

    if (s_gates[index].suspendCount == 0)
    {
        // Nothing is being asked of the thread, so there is nothing to wait for.
        acknowledged = true;
    }
    else
    {
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(msec / 1000u);
        deadline.tv_nsec += (long)(msec % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        while (!s_gates[index].parked && s_gates[index].suspendCount > 0)
        {
            if (pthread_cond_timedwait(&s_gates[index].cond, &s_gates[index].mutex, &deadline) == ETIMEDOUT)
                break;
        }

        acknowledged = s_gates[index].parked;
    }

    pthread_mutex_unlock(&s_gates[index].mutex);
    return acknowledged;
}

// ---------------------------------------------------------------------------
// Recursive critical sections
// ---------------------------------------------------------------------------

namespace
{
    constexpr int kMaxCriticalSections = 64;

    pthread_mutex_t s_critMutex[kMaxCriticalSections];
    thread_local int tls_critDepth[kMaxCriticalSections];
    int s_critCount = 0;

    uint32_t s_acquisitions = 0;
}

void Posix_InitCriticalSections(int count)
{
    if (count > kMaxCriticalSections)
        count = kMaxCriticalSections;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    for (int i = 0; i < count; ++i)
    {
        pthread_mutex_init(&s_critMutex[i], &attr);
        tls_critDepth[i] = 0;
    }

    pthread_mutexattr_destroy(&attr);
    s_critCount = count;
}

void Posix_LockCriticalSection(int index)
{
    if (index < 0 || index >= s_critCount)
        return;

    pthread_mutex_lock(&s_critMutex[index]);
    ++tls_critDepth[index];
    __atomic_add_fetch(&s_acquisitions, 1, __ATOMIC_RELAXED);
}

void Posix_UnlockCriticalSection(int index)
{
    if (index < 0 || index >= s_critCount)
        return;

    if (tls_critDepth[index] > 0)
        --tls_critDepth[index];

    pthread_mutex_unlock(&s_critMutex[index]);
}

int Posix_CriticalSectionDepth(int index)
{
    if (index < 0 || index >= s_critCount)
        return 0;
    return tls_critDepth[index];
}

uint32_t Posix_CriticalSectionAcquisitions()
{
    return __atomic_load_n(&s_acquisitions, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// Physical memory arena
// ---------------------------------------------------------------------------

bool Posix_ArenaReserve(uint32_t size, uint8_t **baseOut, uint32_t *pageSizeOut)
{
    if (!baseOut)
    {
        errno = EINVAL;
        return false;
    }

    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSizeOut)
        *pageSizeOut = (pageSize > 0) ? (uint32_t)pageSize : 4096u;

    // PROT_NONE: address space only. Any access before commit faults rather than
    // silently reading zeros.
    void *reserved = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved == MAP_FAILED)
    {
        *baseOut = nullptr;
        return false;
    }

    *baseOut = (uint8_t *)reserved;
    return true;
}

bool Posix_ArenaCommit(uint8_t *base, uint32_t arenaSize, uint32_t *committedBytes, uint32_t offset, uint32_t length)
{
    if (!base || !committedBytes)
    {
        errno = EINVAL;
        return false;
    }

    // Check the requested range against the arena before any rounding, so an
    // overflowing offset+length cannot wrap into a valid-looking range.
    const uint64_t end = (uint64_t)offset + (uint64_t)length;
    if (end > (uint64_t)arenaSize)
    {
        errno = ENOMEM;
        return false;
    }

    const long pageSizeRaw = sysconf(_SC_PAGESIZE);
    const uint32_t pageSize = (pageSizeRaw > 0) ? (uint32_t)pageSizeRaw : 4096u;

    // Round the committed watermark up to a page boundary.
    uint32_t needed = (uint32_t)end;
    const uint32_t remainder = needed % pageSize;
    if (remainder != 0)
        needed += pageSize - remainder;

    if (needed > arenaSize)
        needed = arenaSize;

    if (needed <= *committedBytes)
        return true; // already writable

    const uint32_t from = *committedBytes;
    const uint32_t commitLength = needed - from;

    if (mprotect(base + from, commitLength, PROT_READ | PROT_WRITE) != 0)
        return false;

    *committedBytes = needed;
    return true;
}

void Posix_ArenaRelease(uint8_t *base, uint32_t size)
{
    if (!base)
        return;
    munmap(base, size);
}
