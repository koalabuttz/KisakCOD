#include <universal/q_shared.h>

#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <universal/assertive.h>
#include <universal/profile.h>

#include "sys_posix.h"
#ifdef KISAK_SP
#include <win32/win_local.h> // CRITSECT_START_SERVER, Sys_EnterCriticalSection
#endif

uint32_t Win_InitThreads();

#include <pthread.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Engine thread state. Types are fixed by <qcommon/threads.h>.
// ---------------------------------------------------------------------------

void *g_threadValues[THREAD_CONTEXT_COUNT][4];
DWORD threadId[THREAD_CONTEXT_COUNT];
HANDLE threadHandle[THREAD_CONTEXT_COUNT];

uint32_t s_affinityMaskForProcess;
uint32_t s_cpuCount;
uint32_t s_affinityMaskForCpu[4];

// Per-thread TLS block. Com_Init parks its jmp_buf at index 2, so the
// [context][4] shape must be preserved exactly.
static thread_local void **g_threadLocals;

static int g_databaseThreadOwner;
static int32_t s_renderPausedCount;
static WinThreadLock s_threadLock;

bool dediRenderHack = false;

#ifdef KISAK_SP
// Singleplayer's contexts (gfx_d3d/rb_backend.h): it adds a third worker, the
// server thread, the stream and sound callback threads and the server demo
// thread. Names as in src/qcommon/threads.cpp.
static const char *s_threadNames[THREAD_CONTEXT_COUNT] =
{
    "MAIN",
    "BACKEND",
    "WORKER0",
    "WORKER1",
    "WORKER2",
    "SERVER",
    "CINEMATIC",
    "TITLE_SERVER",
    "DATABASE",
    "STREAM",
    "SNDSTREAMPACKETCALLBACK",
    "SERVER_DEMO"
};
#else
static const char *s_threadNames[THREAD_CONTEXT_COUNT] =
{
    "Main",
    "Backend",
    "Worker0",
    "Worker1",
    "Cinematic",
    "Titleserver",
    "Database",
};
#endif

namespace
{
    struct ThreadSlot
    {
        pthread_t tid;
        uint32_t id;
        void (*func)(uint32_t);
        int context;
    };

    ThreadSlot s_threadSlots[THREAD_CONTEXT_COUNT];
    thread_local int tls_threadContext = -1;

    pthread_mutex_t s_eventCreateLock = PTHREAD_MUTEX_INITIALIZER;

    // How long a caller asking for quiescence waits for a cooperating thread to
    // reach a checkpoint. Deliberately bounded: a thread that is not waiting
    // anywhere cannot be forced to stop on Linux.
    constexpr uint32_t kSuspendAckTimeoutMsec = 250;

    // Same, for the shutdown and fatal-error path that parks every other thread:
    // several threads at the full budget would delay the report the caller is
    // trying to make.
    constexpr uint32_t kSuspendOtherAckTimeoutMsec = 50;
}

// Lazily create events the engine sometimes touches before spawning a thread.
static void EnsureEvent(PosixEvent **slot, bool manualReset, bool initialState)
{
    if (*slot)
        return;

    pthread_mutex_lock(&s_eventCreateLock);
    if (!*slot)
        *slot = Posix_CreateEvent(manualReset, initialState);
    pthread_mutex_unlock(&s_eventCreateLock);
}

// ---------------------------------------------------------------------------
// Thread identity and TLS
// ---------------------------------------------------------------------------

uint32_t Sys_GetCpuCount()
{
    return s_cpuCount;
}

uint32_t Sys_GetCurrentThreadId()
{
    return (uint32_t)(uintptr_t)pthread_self();
}

void Sys_InitMainThread()
{
    threadId[THREAD_CONTEXT_MAIN] = Sys_GetCurrentThreadId();
    s_threadSlots[THREAD_CONTEXT_MAIN].tid = pthread_self();
    s_threadSlots[THREAD_CONTEXT_MAIN].id = threadId[THREAD_CONTEXT_MAIN];

    Posix_InitSuspendGates(THREAD_CONTEXT_COUNT);
    Win_InitThreads();

    g_threadLocals = g_threadValues[THREAD_CONTEXT_MAIN];
    tls_threadContext = THREAD_CONTEXT_MAIN;
    Com_InitThreadData(THREAD_CONTEXT_MAIN);
}

void Sys_InitThread(ThreadContext_t threadContext)
{
    g_threadLocals = g_threadValues[threadContext];
    tls_threadContext = (int)threadContext;
    Com_InitThreadData(threadContext);
    Profile_InitContext(threadContext);
}

void SetThreadName(uint32_t threadId_, const char *threadName)
{
    (void)threadId_;
    if (!threadName)
        return;

    char name[16]; // Linux limit includes the terminator
    strncpy(name, threadName, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
#if defined(__APPLE__)
    // macOS only names the calling thread, which is the thread asked about here.
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

static void *ThreadTrampoline(void *argument)
{
    const int context = (int)(intptr_t)argument;

    threadId[context] = Sys_GetCurrentThreadId();
    s_threadSlots[context].id = threadId[context];

    SetThreadName(0xFFFFFFFFu, s_threadNames[context]);

    // The thread was created suspended, so it stops here until the spawner
    // resumes it. Nothing engine-visible has run yet, which is what the Windows
    // build gets from CreateThread(..., CREATE_SUSPENDED).
    Posix_SuspendCheckpoint(context);

    Sys_InitThread((ThreadContext_t)context);

    // A suspend requested while the thread was initialising must still hold it
    // back from the thread function.
    Posix_SuspendCheckpoint(context);

    s_threadSlots[context].func((uint32_t)context);
    return nullptr;
}

void Sys_CreateThread(void (*function)(uint32_t), ThreadContext_t threadContext)
{
    iassert(function != nullptr);
    iassert(threadContext < THREAD_CONTEXT_COUNT);

    ThreadSlot &slot = s_threadSlots[threadContext];
    slot.func = function;
    slot.context = (int)threadContext;

    // Create the thread suspended: latch the request on the gate before the
    // pthread exists, so the trampoline cannot run the callback before the
    // spawner resumes it. Spawners that want it running immediately call
    // Sys_ResumeThread; worker threads stay parked until their caller resumes.
    Posix_RequestSuspend((int)threadContext);

    if (pthread_create(&slot.tid, nullptr, ThreadTrampoline, (void *)(intptr_t)threadContext) != 0)
    {
        // Nothing owns the gate now, so drop the request rather than leaving a
        // context that looks permanently suspended.
        Posix_RequestResume((int)threadContext);
        threadHandle[threadContext] = nullptr;
        return;
    }

    threadHandle[threadContext] = (HANDLE)(intptr_t)(threadContext + 1);
}

void Sys_SuspendThread(ThreadContext_t threadContext)
{
    iassert(threadHandle[threadContext] != nullptr);
    Posix_RequestSuspend((int)threadContext);

    // Win32's SuspendThread returns once the thread has actually stopped, so give
    // it a bounded chance to reach a checkpoint. Bounded because a thread busy
    // away from any checkpoint cannot be forced to stop, and a caller suspending
    // itself would park forever.
    if (tls_threadContext != (int)threadContext)
        Posix_WaitForSuspendAck((int)threadContext, kSuspendAckTimeoutMsec);
}

void Sys_ResumeThread(ThreadContext_t threadContext)
{
    iassert(threadHandle[threadContext] != nullptr);
    Posix_RequestResume((int)threadContext);
}

void Sys_SuspendOtherThreads()
{
    const uint32_t currentThreadId = Sys_GetCurrentThreadId();
    for (int threadIndex = 0; threadIndex < THREAD_CONTEXT_COUNT; ++threadIndex)
    {
        if (threadHandle[threadIndex] && threadId[threadIndex] && threadId[threadIndex] != currentThreadId)
            Posix_RequestSuspend(threadIndex);
    }

    // Callers use this to get the other threads out of the way (fatal error
    // reporting, shutdown). Give each one a bounded chance to park so the
    // quiescence the Windows build gets from SuspendThread is mostly preserved.
    for (int threadIndex = 0; threadIndex < THREAD_CONTEXT_COUNT; ++threadIndex)
    {
        if (threadHandle[threadIndex] && threadId[threadIndex] && threadId[threadIndex] != currentThreadId)
            Posix_WaitForSuspendAck(threadIndex, kSuspendOtherAckTimeoutMsec);
    }
}

// ---------------------------------------------------------------------------
// Event wrappers
// ---------------------------------------------------------------------------

void Sys_CreateEvent(bool manualReset, bool initialState, void **event)
{
    *event = Posix_CreateEvent(manualReset, initialState);
}

void Sys_SetEvent(void **event)
{
    if (event && *event)
        Posix_SetEvent((PosixEvent *)*event);
}

void Sys_ResetEvent(void **event)
{
    if (event && *event)
        Posix_ResetEvent((PosixEvent *)*event);
}

void Sys_WaitForSingleObject(void **event)
{
    if (!event || !*event)
        return;

    // Honour a pending request before blocking, and again once the wait is over:
    // a thread that was asleep when the request arrived has to stop here rather
    // than run on unnoticed.
    Posix_SuspendCheckpoint(tls_threadContext);
    Posix_WaitEventInfinite((PosixEvent *)*event);
    Posix_SuspendCheckpoint(tls_threadContext);
}

bool Sys_WaitForSingleObjectTimeout(void **event, uint32_t msec)
{
    iassert(msec != 0xFFFFFFFFu);
    if (!event || !*event)
        return false;

    Posix_SuspendCheckpoint(tls_threadContext);
    const bool signalled = Posix_WaitEvent((PosixEvent *)*event, msec);
    // Also covers a timeout: a polling loop that keeps timing out would otherwise
    // never stop for a suspend request.
    Posix_SuspendCheckpoint(tls_threadContext);
    return signalled;
}

void Sys_SetValue(int valueIndex, void *data)
{
    iassert(valueIndex >= 0 && valueIndex < 4);
    g_threadLocals[valueIndex] = data;
}

void *Sys_GetValue(int valueIndex)
{
    iassert(valueIndex >= 0 && valueIndex < 4);
    return g_threadLocals[valueIndex];
}

bool Sys_IsMainThread()
{
    return Sys_GetCurrentThreadId() == threadId[THREAD_CONTEXT_MAIN];
}

bool Sys_IsRenderThread()
{
    return Sys_GetCurrentThreadId() == threadId[THREAD_CONTEXT_BACKEND];
}

bool Sys_IsDatabaseThread()
{
    return Sys_GetCurrentThreadId() == threadId[THREAD_CONTEXT_DATABASE];
}

// ---------------------------------------------------------------------------
// CPU topology
// ---------------------------------------------------------------------------

uint32_t Win_InitThreads()
{
    long detected = sysconf(_SC_NPROCESSORS_ONLN);
    if (detected < 1)
        detected = 1;

    // The engine models at most four affinity groups.
    s_cpuCount = (detected > 4) ? 4u : (uint32_t)detected;

    s_affinityMaskForProcess = (s_cpuCount >= 32) ? 0xFFFFFFFFu : ((1u << s_cpuCount) - 1u);
    for (uint32_t i = 0; i < 4; ++i)
        s_affinityMaskForCpu[i] = 1u << (i < s_cpuCount ? i : s_cpuCount - 1);

    return s_affinityMaskForProcess;
}

void Win_SetThreadLock(WinThreadLock threadLock)
{
    // Affinity pinning is only a scheduling hint; forcing it on Linux fights the
    // scheduler for no correctness benefit, so the request is recorded only.
    s_threadLock = threadLock;
}

WinThreadLock Win_GetThreadLock()
{
    return s_threadLock;
}

void Win_UpdateThreadLock()
{
}

void Sys_EndLoadThreadPriorities()
{
    iassert(Sys_IsMainThread());
}

void Sys_BeginLoadThreadPriorities()
{
}

// ---------------------------------------------------------------------------
// Render / backend / worker events
// ---------------------------------------------------------------------------

static PosixEvent *renderPausedEvent;
static PosixEvent *renderCompletedEvent;
static PosixEvent *noThreadOwnershipEvent;
static PosixEvent *rendererRunningEvent;
static PosixEvent *backendEvent[2];
static PosixEvent *workerCmdEvent;
static PosixEvent *updateSpotLightEffectEvent;
static PosixEvent *updateEffectsEvent;

static volatile void *smpData;

char Sys_SpawnRenderThread(void (*function)(uint32_t))
{
    renderPausedEvent = Posix_CreateEvent(false, false);
    renderCompletedEvent = Posix_CreateEvent(true, true);
    noThreadOwnershipEvent = Posix_CreateEvent(true, false);
    rendererRunningEvent = Posix_CreateEvent(true, true);
    backendEvent[1] = Posix_CreateEvent(false, false);
    backendEvent[0] = Posix_CreateEvent(true, false);
    updateSpotLightEffectEvent = Posix_CreateEvent(true, true);
    updateEffectsEvent = Posix_CreateEvent(true, true);

    Sys_CreateThread(function, THREAD_CONTEXT_BACKEND);

    if (!threadHandle[THREAD_CONTEXT_BACKEND])
        return 0;

    Sys_ResumeThread(THREAD_CONTEXT_BACKEND);
    return 1;
}

bool Sys_SpawnWorkerThread(void (*function)(uint32_t), uint32_t threadIndex)
{
    const ThreadContext_t threadContext = (ThreadContext_t)(threadIndex + 2);
    iassert(!threadHandle[threadContext]);

    EnsureEvent(&workerCmdEvent, true, false);

    // The worker is left suspended, as on Windows: the caller decides when it
    // starts by calling Sys_ResumeThread once the worker is wanted.
    Sys_CreateThread(function, threadContext);
    return threadHandle[threadContext] != nullptr;
}

int Sys_IsRendererReady()
{
    return Sys_WaitForSingleObjectTimeout((void **)&renderCompletedEvent, 0);
}

int Sys_IsMainThreadReady()
{
    return Sys_WaitForSingleObjectTimeout((void **)&noThreadOwnershipEvent, 0);
}

bool Sys_FinishRenderer()
{
    return !Sys_WaitForSingleObjectTimeout((void **)&noThreadOwnershipEvent, 0);
}

void Sys_ReleaseThreadOwnership()
{
    EnsureEvent(&noThreadOwnershipEvent, true, false);
    Sys_SetEvent((void **)&noThreadOwnershipEvent);
}

void Sys_WaitForMainThread()
{
    EnsureEvent(&noThreadOwnershipEvent, true, false);
    Sys_WaitForSingleObject((void **)&noThreadOwnershipEvent);
}

void *Sys_RendererSleep()
{
    return (void *)__atomic_exchange_n(&smpData, (void *)nullptr, __ATOMIC_SEQ_CST);
}

int Sys_RendererReady()
{
    return __atomic_load_n(&smpData, __ATOMIC_SEQ_CST) != nullptr;
}

void Sys_RenderCompleted()
{
    EnsureEvent(&renderCompletedEvent, true, true);
    Sys_SetEvent((void **)&renderCompletedEvent);
    Sys_SetWorkerCmdEvent();
}

void Sys_FrontEndSleep()
{
    EnsureEvent(&noThreadOwnershipEvent, true, false);
    EnsureEvent(&rendererRunningEvent, true, true);
    EnsureEvent(&backendEvent[1], false, false);

    Sys_WaitForSingleObject((void **)&rendererRunningEvent);
    Sys_ResetEvent((void **)&noThreadOwnershipEvent);
    Sys_SetEvent((void **)&backendEvent[1]);

    const int32_t newCount = __atomic_sub_fetch(&s_renderPausedCount, 1, __ATOMIC_SEQ_CST);
    iassert(newCount == -1 || newCount == 0);

    Sys_WaitForSingleObject((void **)&renderPausedEvent);
}

void Sys_WakeRenderer(void *data)
{
    EnsureEvent(&renderCompletedEvent, true, true);
    EnsureEvent(&backendEvent[1], false, false);

    Sys_ResetEvent((void **)&renderCompletedEvent);
    // A frame belongs to the backend until it takes the pointer. Reject a
    // second submission rather than losing the first, including in builds
    // where assertions are disabled or the assertion handler returns.
    volatile void *pending = nullptr;
    if (!__atomic_compare_exchange_n(&smpData, &pending, data, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        iassert(pending == nullptr);
        return;
    }

    Sys_SetEvent((void **)&backendEvent[1]);
    Sys_SetWorkerCmdEvent();
}

void Sys_NotifyRenderer()
{
    EnsureEvent(&backendEvent[1], false, false);
    Sys_SetEvent((void **)&backendEvent[1]);
}

void Sys_StopRenderer()
{
    EnsureEvent(&rendererRunningEvent, true, true);
    EnsureEvent(&renderPausedEvent, false, false);

    const int32_t newCount = __atomic_add_fetch(&s_renderPausedCount, 1, __ATOMIC_SEQ_CST);
    iassert(newCount == 0 || newCount == 1);

    Sys_ResetEvent((void **)&rendererRunningEvent);
    Sys_SetEvent((void **)&renderPausedEvent);
}

void Sys_StartRenderer()
{
    EnsureEvent(&rendererRunningEvent, true, true);
    Sys_SetEvent((void **)&rendererRunningEvent);
}

int Sys_WaitBackendEvent()
{
    EnsureEvent(&backendEvent[1], false, false);
    return Sys_WaitForSingleObjectTimeout((void **)&backendEvent[1], 0);
}

void Sys_WaitForWorkerCmd()
{
    // A 1 ms timed wait, as on Windows (Sys_WaitForSingleObjectTimeout(backendEvent,
    // 1)), never an unbounded one. Its callers re-check their own condition after
    // it: the render thread's R_ProcessWorkerCmdsWithTimeout(Sys_WaitBackendEvent, 1)
    // looks at the backend event, which Sys_FrontEndSleep sets without touching
    // this event. Sleeping until this event alone was set left the frontend and
    // the backend waiting on each other at the first Com_SyncThreads.
    EnsureEvent(&workerCmdEvent, true, false);
    Sys_WaitForSingleObjectTimeout((void **)&workerCmdEvent, 1);
}

void Sys_SetWorkerCmdEvent()
{
    EnsureEvent(&workerCmdEvent, true, false);
    Sys_SetEvent((void **)&workerCmdEvent);
}

void Sys_ResetWorkerCmdEvent()
{
    EnsureEvent(&workerCmdEvent, true, false);
    Sys_ResetEvent((void **)&workerCmdEvent);
}

void Sys_SetUpdateSpotLightEffectEvent()
{
    EnsureEvent(&updateSpotLightEffectEvent, true, true);
    Sys_SetEvent((void **)&updateSpotLightEffectEvent);
}

void Sys_ResetUpdateSpotLightEffectEvent()
{
    EnsureEvent(&updateSpotLightEffectEvent, true, true);
    Sys_ResetEvent((void **)&updateSpotLightEffectEvent);
}

void Sys_WaitUpdateNonDependentEffectsCompleted()
{
    EnsureEvent(&updateEffectsEvent, true, true);
    Sys_WaitForSingleObject((void **)&updateEffectsEvent);
}

void Sys_SetUpdateNonDependentEffectsEvent()
{
    EnsureEvent(&updateEffectsEvent, true, true);
    Sys_SetEvent((void **)&updateEffectsEvent);
}

void Sys_ResetUpdateNonDependentEffectsEvent()
{
    EnsureEvent(&updateEffectsEvent, true, true);
    Sys_ResetEvent((void **)&updateEffectsEvent);
}

// ---------------------------------------------------------------------------
// Database thread
// ---------------------------------------------------------------------------

static PosixEvent *wakeDatabaseEvent;
static PosixEvent *databaseCompletedEvent;
static PosixEvent *databaseCompletedEvent2;
static PosixEvent *resumedDatabaseEvent;

char Sys_SpawnDatabaseThread(void (*function)(uint32_t))
{
    wakeDatabaseEvent = Posix_CreateEvent(false, false);
    databaseCompletedEvent = Posix_CreateEvent(true, true);
    databaseCompletedEvent2 = Posix_CreateEvent(true, true);
    resumedDatabaseEvent = Posix_CreateEvent(true, true);

    Sys_CreateThread(function, THREAD_CONTEXT_DATABASE);

    if (!threadHandle[THREAD_CONTEXT_DATABASE])
        return 0;

    Sys_ResumeThread(THREAD_CONTEXT_DATABASE);
    return 1;
}

void Sys_SuspendDatabaseThread(ThreadOwner owner)
{
    iassert(owner != THREAD_OWNER_NONE);
    iassert(g_databaseThreadOwner == THREAD_OWNER_NONE);

    g_databaseThreadOwner = owner;
    EnsureEvent(&resumedDatabaseEvent, true, true);
    Sys_ResetEvent((void **)&resumedDatabaseEvent);
}

void Sys_ResumeDatabaseThread(ThreadOwner owner)
{
    iassert(owner != THREAD_OWNER_NONE);
    iassert(g_databaseThreadOwner == owner);

    g_databaseThreadOwner = THREAD_OWNER_NONE;
    Sys_SetEvent((void **)&resumedDatabaseEvent);
}

bool Sys_HaveSuspendedDatabaseThread(ThreadOwner owner)
{
    return g_databaseThreadOwner == owner;
}

void Sys_WaitDatabaseThread()
{
    EnsureEvent(&resumedDatabaseEvent, true, true);
    Sys_WaitForSingleObject((void **)&resumedDatabaseEvent);
}

#ifdef KISAK_SP
// Singleplayer's server thread (below) must not start a server while the
// database thread is registering assets; threads.cpp keeps this flag.
int isDoingDatabaseInit;
static PosixEvent *serverCompletedEvent;
#endif

void Sys_DatabaseCompleted()
{
#ifdef KISAK_SP
    // As threads.cpp: hold off the server thread, then wait for it to finish
    // the frame it is in.
    Sys_EnterCriticalSection(CRITSECT_START_SERVER);
    isDoingDatabaseInit = 1;
    Sys_LeaveCriticalSection(CRITSECT_START_SERVER);
    Sys_WaitForSingleObject((void **)&serverCompletedEvent);
#endif
    EnsureEvent(&databaseCompletedEvent, true, true);
    Sys_SetEvent((void **)&databaseCompletedEvent);
}

void Sys_WaitStartDatabase()
{
    EnsureEvent(&wakeDatabaseEvent, false, false);
    Sys_WaitForSingleObject((void **)&wakeDatabaseEvent);
}

bool Sys_IsDatabaseReady()
{
    return Sys_WaitForSingleObjectTimeout((void **)&databaseCompletedEvent, 0);
}

void Sys_SyncDatabase()
{
    EnsureEvent(&databaseCompletedEvent, true, true);
    Sys_WaitForSingleObject((void **)&databaseCompletedEvent);
}

void Sys_WakeDatabase()
{
    EnsureEvent(&databaseCompletedEvent, true, true);
    Sys_ResetEvent((void **)&databaseCompletedEvent);
}

void Sys_NotifyDatabase()
{
    EnsureEvent(&wakeDatabaseEvent, false, false);
    Sys_SetEvent((void **)&wakeDatabaseEvent);
}

void Sys_DatabaseCompleted2()
{
#ifdef KISAK_SP
    Sys_EnterCriticalSection(CRITSECT_START_SERVER);
    isDoingDatabaseInit = 0;
    Sys_LeaveCriticalSection(CRITSECT_START_SERVER);
#endif
    EnsureEvent(&databaseCompletedEvent2, true, true);
    Sys_SetEvent((void **)&databaseCompletedEvent2);
}

bool Sys_IsDatabaseReady2()
{
    return Sys_WaitForSingleObjectTimeout((void **)&databaseCompletedEvent2, 0);
}

void Sys_WakeDatabase2()
{
    EnsureEvent(&databaseCompletedEvent2, true, true);
    Sys_ResetEvent((void **)&databaseCompletedEvent2);
}

// ---------------------------------------------------------------------------
// Cinematics thread
// ---------------------------------------------------------------------------

static PosixEvent *g_cinematicsThreadOutstandingRequestEvent;
static PosixEvent *g_cinematicsHostOutstandingRequestEvent;

char Sys_SpawnCinematicsThread(void (*function)(uint32_t))
{
    g_cinematicsThreadOutstandingRequestEvent = Posix_CreateEvent(true, true);
    g_cinematicsHostOutstandingRequestEvent = Posix_CreateEvent(true, false);

    Sys_CreateThread(function, THREAD_CONTEXT_CINEMATIC);
    if (!threadHandle[THREAD_CONTEXT_CINEMATIC])
        return 0;

    Sys_ResumeThread(THREAD_CONTEXT_CINEMATIC);
    return 1;
}

bool Sys_WaitForCinematicsThreadOutstandingRequestEventTimeout(uint32_t timeoutMsec)
{
    return Sys_WaitForSingleObjectTimeout((void **)&g_cinematicsThreadOutstandingRequestEvent, timeoutMsec);
}

void Sys_SetCinematicsThreadOutstandingRequestEvent()
{
    EnsureEvent(&g_cinematicsThreadOutstandingRequestEvent, true, true);
    Sys_SetEvent((void **)&g_cinematicsThreadOutstandingRequestEvent);
}

void Sys_ResetCinematicsThreadOutstandingRequestEvent()
{
    EnsureEvent(&g_cinematicsThreadOutstandingRequestEvent, true, true);
    Sys_ResetEvent((void **)&g_cinematicsThreadOutstandingRequestEvent);
}

bool Sys_WaitForCinematicsHostOutstandingRequestEventTimeout(uint32_t timeoutMsec)
{
    return Sys_WaitForSingleObjectTimeout((void **)&g_cinematicsHostOutstandingRequestEvent, timeoutMsec);
}

void Sys_SetCinematicsHostOutstandingRequestEvent()
{
    EnsureEvent(&g_cinematicsHostOutstandingRequestEvent, true, false);
    Sys_SetEvent((void **)&g_cinematicsHostOutstandingRequestEvent);
}

void Sys_ResetCinematicsHostOutstandingRequestEvent()
{
    EnsureEvent(&g_cinematicsHostOutstandingRequestEvent, true, false);
    Sys_ResetEvent((void **)&g_cinematicsHostOutstandingRequestEvent);
}

#ifdef KISAK_SP
// ---------------------------------------------------------------------------
// Singleplayer server thread and server demo thread
//
// Singleplayer runs its server in a thread of its own (SV_ServerThread in
// server/sv_main.cpp), paced by these events. Each one mirrors the KISAK_SP
// block of src/qcommon/threads.cpp, with the same reset modes and initial
// states. Windows waits on some of them with WaitForSingleObject directly;
// here every wait goes through the suspend-aware wrappers above.
// ---------------------------------------------------------------------------

static PosixEvent *wakeServerEvent;
static PosixEvent *allowSendClientMessagesEvent;
static PosixEvent *serverSnapshotEvent;
static PosixEvent *clientMessageReceived;
static PosixEvent *g_saveHistoryEvent;
static PosixEvent *g_saveHistoryDoneEvent;

volatile int g_timeout;

bool Sys_IsServerThread()
{
    return threadId[THREAD_CONTEXT_SERVER] == Sys_GetCurrentThreadId();
}

int Sys_WaitStartServer(uint32_t timeout)
{
    Sys_EnterCriticalSection(CRITSECT_START_SERVER);
    int started = Sys_WaitForSingleObjectTimeout((void **)&wakeServerEvent, timeout);
    if (isDoingDatabaseInit)
        started = 0;
    else if (started)
        Sys_ResetEvent((void **)&serverCompletedEvent);
    Sys_LeaveCriticalSection(CRITSECT_START_SERVER);
    return started;
}

void Sys_InitServerEvents()
{
    Sys_ResetEvent((void **)&wakeServerEvent);
    Sys_ResetEvent((void **)&serverCompletedEvent);
    Sys_SetEvent((void **)&allowSendClientMessagesEvent);
    Sys_ResetEvent((void **)&serverSnapshotEvent);
    Sys_SetEvent((void **)&clientMessageReceived);
    g_timeout = 0;
}

void Sys_ClientMessageReceived()
{
    Sys_SetEvent((void **)&clientMessageReceived);
}

void Sys_ClearClientMessage()
{
    Sys_ResetEvent((void **)&clientMessageReceived);
}

int Sys_SpawnServerThread(void (*function)(uint32_t))
{
    wakeServerEvent = Posix_CreateEvent(true, false);
    serverCompletedEvent = Posix_CreateEvent(true, false);
    allowSendClientMessagesEvent = Posix_CreateEvent(true, false);
    serverSnapshotEvent = Posix_CreateEvent(false, false);
    clientMessageReceived = Posix_CreateEvent(true, true);
    Sys_CreateThread(function, THREAD_CONTEXT_SERVER);
    if (!threadHandle[THREAD_CONTEXT_SERVER])
        return 0;
    Sys_ResumeThread(THREAD_CONTEXT_SERVER);
    return 1;
}

void Sys_WaitClientMessageReceived()
{
    PROF_SCOPED("wait receive msg");
    Sys_WaitForSingleObject((void **)&clientMessageReceived);
}

void Sys_ServerSnapshotCompleted()
{
    Sys_SetEvent((void **)&serverSnapshotEvent);
}

bool Sys_WaitServerSnapshot()
{
    PROF_SCOPED("wait snapshot");
    return Sys_WaitForSingleObjectTimeout((void **)&serverSnapshotEvent, 1);
}

void Sys_AllowSendClientMessages()
{
    Sys_SetEvent((void **)&allowSendClientMessagesEvent);
}

void Sys_DisallowSendClientMessages()
{
    Sys_ResetEvent((void **)&allowSendClientMessagesEvent);
}

int Sys_CanSendClientMessages()
{
    return Sys_WaitForSingleObjectTimeout((void **)&allowSendClientMessagesEvent, 0);
}

void Sys_ServerCompleted()
{
    Sys_SetEvent((void **)&serverCompletedEvent);
}

int Sys_ServerTimeout()
{
    const int time = g_timeout;
    if (!time)
        return 1;

    const int timeMS = Sys_Milliseconds();
    if (timeMS - time >= 0)
    {
        // As threads.cpp: move the deadline on by one 50 ms server frame, unless
        // another thread already moved it.
        const int nextTimeout = timeMS - (int)(-50.0f / com_timescaleValue);
        __sync_bool_compare_and_swap(&g_timeout, time, nextTimeout);
        return 1;
    }
    return 0;
}

void Sys_WakeServer()
{
    Sys_SetEvent((void **)&wakeServerEvent);
}

bool Sys_WaitServer()
{
    PROF_SCOPED("wait server");
    return Sys_WaitForSingleObjectTimeout((void **)&serverCompletedEvent, 1);
}

void Sys_SleepServer()
{
    if (Sys_WaitForSingleObjectTimeout((void **)&wakeServerEvent, 0))
    {
        Sys_EnterCriticalSection(CRITSECT_START_SERVER);
        Sys_ResetEvent((void **)&wakeServerEvent);
        Sys_LeaveCriticalSection(CRITSECT_START_SERVER);
    }
}

void Sys_Sleep(uint32_t msec)
{
    usleep(msec * 1000u);
}

void Sys_SetServerTimeout(int timeout)
{
    iassert(timeout >= 0);
    iassert(com_timescaleValue);

    if (timeout)
    {
        const int val = (int)((float)timeout / com_timescaleValue);
        const int timeMS = Sys_Milliseconds();
        if (!(g_timeout && timeMS - g_timeout < 0 && g_timeout - (timeMS + val) <= 0))
            g_timeout = timeMS + val;
    }
    else
    {
        g_timeout = 0;
    }
}

bool Sys_WaitForSaveHistoryDone()
{
    return Sys_WaitForSingleObjectTimeout((void **)&g_saveHistoryDoneEvent, 2000);
}

int Sys_SpawnServerDemoThread(void (*function)(uint32_t))
{
    g_saveHistoryEvent = Posix_CreateEvent(false, false);
    g_saveHistoryDoneEvent = Posix_CreateEvent(false, false);
    Sys_CreateThread(function, THREAD_CONTEXT_SERVER_DEMO);
    if (!threadHandle[THREAD_CONTEXT_SERVER_DEMO])
        return 0;
    Sys_ResumeThread(THREAD_CONTEXT_SERVER_DEMO);
    return 1;
}

void Sys_SetSaveHistoryEvent()
{
    Sys_SetEvent((void **)&g_saveHistoryEvent);
}

void Sys_WaitForSaveHistory()
{
    Sys_WaitForSingleObject((void **)&g_saveHistoryEvent);
}

void Sys_SetSaveHistoryDoneEvent()
{
    Sys_SetEvent((void **)&g_saveHistoryDoneEvent);
}
#endif // KISAK_SP
