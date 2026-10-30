// Standalone self-test for the engine-free POSIX runtime primitives.
//
// Build and run without the engine (see sys_posix.h for the primitives):
//   g++ -std=c++20 -DSYS_POSIX_SELFTEST -pthread
//       src/linux/sys_posix.cpp src/linux/sys_selftest.cpp -o /tmp/sys_selftest
//   /tmp/sys_selftest
//
// The whole file is compiled out unless SYS_POSIX_SELFTEST is defined, so it can
// safely sit in the platform source directory without polluting the server build.

#ifdef SYS_POSIX_SELFTEST

#include "sys_posix.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    int s_failures = 0;
    int s_checks = 0;

    void Check(bool condition, const char *what)
    {
        ++s_checks;
        if (!condition)
        {
            ++s_failures;
            printf("  FAIL: %s\n", what);
        }
    }

    // --- Event tests -------------------------------------------------------

    void TestManualResetEvent()
    {
        printf("manual-reset event\n");
        PosixEvent *event = Posix_CreateEvent(true, false);
        Check(event != nullptr, "event created");
        Check(!Posix_WaitEvent(event, 20), "unsignalled event times out");

        Posix_SetEvent(event);
        Check(Posix_WaitEvent(event, 0), "signalled event is observed");
        // Manual reset must stay signalled, so a second wait also succeeds.
        Check(Posix_WaitEvent(event, 0), "manual-reset stays signalled");

        Posix_ResetEvent(event);
        Check(!Posix_WaitEvent(event, 20), "reset clears the signal");

        Posix_DestroyEvent(event);
    }

    void TestAutoResetEvent()
    {
        printf("auto-reset event\n");
        PosixEvent *event = Posix_CreateEvent(false, false);

        Check(!Posix_WaitEvent(event, 20), "unsignalled auto-reset times out");

        Posix_SetEvent(event);
        Check(Posix_WaitEvent(event, 0), "first wait consumes the signal");
        // Auto reset consumes on release: the signal must be gone.
        Check(!Posix_WaitEvent(event, 20), "auto-reset consumed the signal");

        // Initial state is honoured.
        PosixEvent *initiallySet = Posix_CreateEvent(false, true);
        Check(Posix_WaitEvent(initiallySet, 0), "initial state is honoured");
        Check(!Posix_WaitEvent(initiallySet, 20), "initial signal consumed once");

        Posix_DestroyEvent(initiallySet);
        Posix_DestroyEvent(event);
    }

    struct ManualResetWaiterArgs
    {
        PosixEvent *event;
        int *arrivals;
        pthread_mutex_t *arrivalMutex;
        int index;
    };

    void *ManualResetWaiter(void *raw)
    {
        ManualResetWaiterArgs *args = (ManualResetWaiterArgs *)raw;
        Posix_WaitEventInfinite(args->event);

        pthread_mutex_lock(args->arrivalMutex);
        ++(*args->arrivals);
        pthread_mutex_unlock(args->arrivalMutex);
        return nullptr;
    }

    void TestManualResetWakesAllWaiters()
    {
        printf("manual-reset wakes every waiter\n");

        constexpr int kWaiters = 4;
        PosixEvent *event = Posix_CreateEvent(true, false);
        pthread_mutex_t arrivalMutex = PTHREAD_MUTEX_INITIALIZER;
        int arrivals = 0;

        ManualResetWaiterArgs args[kWaiters];
        pthread_t threads[kWaiters];

        for (int i = 0; i < kWaiters; ++i)
        {
            args[i].event = event;
            args[i].arrivals = &arrivals;
            args[i].arrivalMutex = &arrivalMutex;
            args[i].index = i;
            pthread_create(&threads[i], nullptr, ManualResetWaiter, &args[i]);
        }

        // Give the waiters time to block, then release them all.
        usleep(50 * 1000);
        Posix_SetEvent(event);

        for (int i = 0; i < kWaiters; ++i)
            pthread_join(threads[i], nullptr);

        Check(arrivals == kWaiters, "all waiters woke on a manual-reset signal");

        pthread_mutex_destroy(&arrivalMutex);
        Posix_DestroyEvent(event);
    }

    void TestTimedWaitAccuracy()
    {
        printf("timed wait\n");
        PosixEvent *event = Posix_CreateEvent(true, false);

        const uint64_t start = Posix_MonotonicNanoseconds();
        const bool signalled = Posix_WaitEvent(event, 60);
        const uint64_t elapsedMsec = (Posix_MonotonicNanoseconds() - start) / 1000000ull;

        Check(!signalled, "timed wait reports timeout");
        Check(elapsedMsec >= 50, "timed wait did not return early");
        Check(elapsedMsec < 2000, "timed wait did not overshoot wildly");

        Posix_DestroyEvent(event);
    }

    // --- Monotonic clock ---------------------------------------------------

    void TestMonotonicClock()
    {
        printf("monotonic clock\n");

        const uint64_t first = Posix_MonotonicNanoseconds();
        usleep(10 * 1000);
        const uint64_t second = Posix_MonotonicNanoseconds();

        Check(second > first, "clock advances");
        const uint64_t deltaMsec = (second - first) / 1000000ull;
        Check(deltaMsec >= 5 && deltaMsec < 2000, "elapsed time is plausible");
    }

    // --- Critical sections -------------------------------------------------

    void TestRecursiveCriticalSection()
    {
        printf("recursive critical section\n");

        constexpr int kSections = 4;
        Posix_InitCriticalSections(kSections);

        Check(Posix_CriticalSectionDepth(0) == 0, "depth starts at zero");

        Posix_LockCriticalSection(0);
        Check(Posix_CriticalSectionDepth(0) == 1, "first lock reports depth 1");

        // The engine re-enters sections it already holds (Com_Printf under
        // CRITSECT_CONSOLE); this must not deadlock.
        Posix_LockCriticalSection(0);
        Posix_LockCriticalSection(0);
        Check(Posix_CriticalSectionDepth(0) == 3, "recursive locks report depth 3");

        Posix_UnlockCriticalSection(0);
        Posix_UnlockCriticalSection(0);
        Check(Posix_CriticalSectionDepth(0) == 1, "unwinds to depth 1");

        Posix_UnlockCriticalSection(0);
        Check(Posix_CriticalSectionDepth(0) == 0, "unwinds to depth 0");

        Check(Posix_CriticalSectionAcquisitions() >= 3, "acquisitions counted");
    }

    // --- Physical memory arena --------------------------------------------

    void TestArena()
    {
        printf("physical memory arena\n");

        constexpr uint32_t kArenaSize = 1024u * 1024u;
        uint8_t *base = nullptr;
        uint32_t pageSize = 0;

        Check(Posix_ArenaReserve(kArenaSize, &base, &pageSize), "arena reserved");
        Check(base != nullptr, "arena base is set");
        Check(pageSize >= 4096, "page size is sane");

        uint32_t committed = 0;

        // Nothing is committed yet: commit a small range and write to it.
        Check(Posix_ArenaCommit(base, kArenaSize, &committed, 0, 16), "commit succeeds");
        Check(committed >= pageSize, "commit rounds up to a page");

        memset(base, 0xAB, 16);
        Check(base[0] == 0xAB && base[15] == 0xAB, "committed memory is writable");

        // A commit inside the already-committed window is a no-op.
        const uint32_t committedBefore = committed;
        Check(Posix_ArenaCommit(base, kArenaSize, &committed, 8, 8), "idempotent commit succeeds");
        Check(committed == committedBefore, "no extra pages committed");

        // Growing the window commits more pages.
        Check(Posix_ArenaCommit(base, kArenaSize, &committed, 4 * pageSize, pageSize), "growth commit succeeds");
        Check(committed > committedBefore, "growth committed more pages");

        // Overruns are rejected, including ones that would wrap offset+length.
        Check(!Posix_ArenaCommit(base, kArenaSize, &committed, kArenaSize, 1), "overrun rejected");
        Check(!Posix_ArenaCommit(base, kArenaSize, &committed, 0xFFFFFFF0u, 0x20), "wrapping range rejected");
        Check(!Posix_ArenaCommit(base, kArenaSize, &committed, kArenaSize - 4, 8), "past-the-end rejected");

        Posix_ArenaRelease(base, kArenaSize);
        printf("  arena released\n");
    }

    // Reserving with PROT_NONE must actually guard the pages: touching memory
    // that was reserved but never committed has to fault, not read zeros.
    void TestArenaFaultsBeforeCommit()
    {
        printf("arena faults before commit\n");

        constexpr uint32_t kArenaSize = 64u * 1024u;
        uint8_t *base = nullptr;
        uint32_t pageSize = 0;
        Check(Posix_ArenaReserve(kArenaSize, &base, &pageSize), "arena reserved for fault test");

        const pid_t child = fork();
        if (child == 0)
        {
            // Child: probe the last byte of the reserved-but-uncommitted arena.
            volatile uint8_t *probe = base + kArenaSize - 1;
            *probe = 1;
            _exit(0); // reached only if the write did NOT fault
        }

        int status = 0;
        waitpid(child, &status, 0);

        const bool faulted = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
        Check(faulted, "reserved-but-uncommitted memory faults on access");

        Posix_ArenaRelease(base, kArenaSize);
    }

    // --- TLS isolation -----------------------------------------------------

    thread_local int tls_value = 0;

    void *TlsWorker(void *raw)
    {
        const int tag = (int)(intptr_t)raw;
        tls_value = tag;
        usleep(10 * 1000); // let other threads write their own copy
        Check(tls_value == tag, "thread-local value is private to its thread");
        return nullptr;
    }

    void TestThreadLocalStorage()
    {
        printf("thread-local storage\n");

        constexpr int kThreads = 4;
        pthread_t threads[kThreads];

        for (int i = 0; i < kThreads; ++i)
            pthread_create(&threads[i], nullptr, TlsWorker, (void *)(intptr_t)(i + 1));

        for (int i = 0; i < kThreads; ++i)
            pthread_join(threads[i], nullptr);

        tls_value = 99;
        Check(tls_value == 99, "main thread keeps its own value");
    }

    // --- Cooperative suspension -------------------------------------------

    struct SuspendedWorkerArgs
    {
        int gate;
        volatile int *workCount;
        volatile bool *sawSuspend;
    };

    void *SuspendedWorker(void *raw)
    {
        SuspendedWorkerArgs *args = (SuspendedWorkerArgs *)raw;

        for (int i = 0; i < 200; ++i)
        {
            if (Posix_IsSuspendRequested(args->gate))
                *args->sawSuspend = true;

            // Checkpoint blocks while a suspend is outstanding.
            Posix_SuspendCheckpoint(args->gate);
            *args->workCount = *args->workCount + 1;
            usleep(1000);
        }
        return nullptr;
    }

    void TestSuspendGate()
    {
        printf("cooperative suspension\n");

        constexpr int kGates = 4;
        Posix_InitSuspendGates(kGates);

        volatile int workCount = 0;
        volatile bool sawSuspend = false;

        SuspendedWorkerArgs args;
        args.gate = 1;
        args.workCount = &workCount;
        args.sawSuspend = &sawSuspend;

        pthread_t worker;
        pthread_create(&worker, nullptr, SuspendedWorker, &args);

        usleep(20 * 1000);

        Posix_RequestSuspend(1);
        Check(Posix_IsSuspendRequested(1), "suspend request is visible");
        usleep(30 * 1000);

        // The worker must be parked inside the checkpoint: its counter stops.
        const int stalled = workCount;
        usleep(50 * 1000);
        Check(workCount == stalled, "suspended thread stops making progress");

        Posix_RequestResume(1);
        Check(!Posix_IsSuspendRequested(1), "resume clears the request");

        pthread_join(worker, nullptr);
        Check(workCount > stalled, "worker resumed and finished");
        Check(sawSuspend, "worker observed the suspend request");
    }

    // Requests are counted, so they nest like a Win32 suspend count, and the
    // gate can be inspected without a thread running on it.
    void TestSuspendNesting()
    {
        printf("suspend nesting\n");

        constexpr int kGates = 4;
        Posix_InitSuspendGates(kGates);

        Posix_RequestSuspend(2);
        Posix_RequestSuspend(2);
        Check(Posix_SuspendCount(2) == 2, "suspend requests nest");
        Check(Posix_IsSuspendRequested(2), "gate reports the outstanding requests");

        Posix_RequestResume(2);
        Check(Posix_SuspendCount(2) == 1, "one resume leaves the gate held");
        Check(Posix_IsSuspendRequested(2), "gate still reports a request");

        Posix_RequestResume(2);
        Check(Posix_SuspendCount(2) == 0, "second resume clears the gate");
        Check(!Posix_IsSuspendRequested(2), "gate reports no request");

        // A released gate must not block a checkpoint.
        const uint64_t beforeCheckpoint = Posix_MonotonicNanoseconds();
        Posix_SuspendCheckpoint(2);
        const uint64_t checkpointMsec = (Posix_MonotonicNanoseconds() - beforeCheckpoint) / 1000000ull;
        Check(checkpointMsec < 100, "checkpoint on a released gate returns immediately");

        // Nothing runs on gate 3, so no thread can acknowledge a request there.
        Posix_RequestSuspend(3);
        Check(!Posix_WaitForSuspendAck(3, 50), "acknowledgement times out when no thread can park");
        Posix_RequestResume(3);
    }

    struct ParkedWorkerArgs
    {
        int gate;
        volatile int *workCount;
        volatile bool *stop;
    };

    void *ParkedWorker(void *raw)
    {
        ParkedWorkerArgs *args = (ParkedWorkerArgs *)raw;

        while (!*args->stop)
        {
            Posix_SuspendCheckpoint(args->gate);
            *args->workCount = *args->workCount + 1;
            usleep(1000);
        }
        return nullptr;
    }

    void TestSuspendAcknowledgement()
    {
        printf("suspend acknowledgement\n");

        constexpr int kGates = 4;
        Posix_InitSuspendGates(kGates);

        volatile int workCount = 0;
        volatile bool stop = false;

        ParkedWorkerArgs args;
        args.gate = 1;
        args.workCount = &workCount;
        args.stop = &stop;

        pthread_t worker;
        pthread_create(&worker, nullptr, ParkedWorker, &args);
        usleep(20 * 1000);

        Posix_RequestSuspend(1);
        Check(Posix_WaitForSuspendAck(1, 500), "parked thread acknowledges the request");

        const int stalled = workCount;
        usleep(30 * 1000);
        Check(workCount == stalled, "acknowledged thread really is stopped");

        // A nested request keeps it stopped even after one resume.
        Posix_RequestSuspend(1);
        Check(Posix_SuspendCount(1) == 2, "second request nests on top of the first");
        Posix_RequestResume(1);
        usleep(30 * 1000);
        Check(workCount == stalled, "still stopped with one request outstanding");

        Posix_RequestResume(1);
        stop = true;
        pthread_join(worker, nullptr);
        Check(workCount > stalled, "thread continues after both resumes");
    }
}

int main()
{
    printf("POSIX runtime self-test\n");
    printf("=======================\n");

    TestMonotonicClock();
    TestManualResetEvent();
    TestAutoResetEvent();
    TestManualResetWakesAllWaiters();
    TestTimedWaitAccuracy();
    TestRecursiveCriticalSection();
    TestArena();
    TestArenaFaultsBeforeCommit();
    TestThreadLocalStorage();
    TestSuspendGate();
    TestSuspendNesting();
    TestSuspendAcknowledgement();

    printf("-----------------------\n");
    printf("%d checks, %d failure(s)\n", s_checks, s_failures);

    return s_failures == 0 ? 0 : 1;
}

#else

// Intentionally empty when the self-test is not enabled.
typedef int sys_selftest_disabled_t;

#endif // SYS_POSIX_SELFTEST
