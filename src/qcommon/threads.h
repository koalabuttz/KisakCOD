#pragma once

#if defined(__linux__) || defined(__APPLE__) || defined(__vita__)
// Native Linux: no Windows SDK. qcommon/threads.h only needs the Win32 base
// types at the bottom of this file (DWORD/HANDLE), which the POSIX bridge
// declares with the fixed 32-bit Windows widths.
#include <linux/compat.h>
#else
#include <windows.h> // literally just for some of the extern types at the bottom
#endif
#include <gfx_d3d/rb_backend.h> // THREAD_CONTEXT_COUNT

enum ThreadOwner : __int32
{                                       // ...
    THREAD_OWNER_NONE = 0x0,
    THREAD_OWNER_DATABASE = 0x1,
    THREAD_OWNER_CINEMATICS = 0x2,
};

uint32_t Sys_GetCpuCount();
void Sys_InitMainThread();
uint32_t Sys_GetCurrentThreadId();
void Sys_InitThread(ThreadContext_t threadContext);
char Sys_SpawnRenderThread(void(* function)(uint32_t));
void Sys_CreateEvent(bool manualReset, bool initialState, void** event);
void Sys_CreateThread(void(* function)(uint32_t), ThreadContext_t threadContext);
void SetThreadName(uint32_t threadId, const char* threadName);
uint32_t __stdcall Sys_ThreadMain(ThreadContext_t parameter);
char Sys_SpawnDatabaseThread(void(* function)(uint32_t));
void Sys_SuspendDatabaseThread(ThreadOwner owner);
void Sys_ResetEvent(void** event);
void Sys_ResumeDatabaseThread(ThreadOwner owner);
void Sys_SetEvent(void** event);
bool Sys_HaveSuspendedDatabaseThread(ThreadOwner owner);
void Sys_WaitDatabaseThread();
void Sys_WaitForSingleObject(void** event);
bool Sys_SpawnWorkerThread(void(* function)(uint32_t), uint32_t threadIndex);
void Sys_SuspendThread(ThreadContext_t threadContext);
void Sys_ResumeThread(ThreadContext_t threadContext);
void *Sys_RendererSleep();
int Sys_RendererReady();
void Sys_RenderCompleted();
void Sys_FrontEndSleep();
bool Sys_WaitForSingleObjectTimeout(void** event, uint32_t msec);
void Sys_WakeRenderer(void* data);
void Sys_NotifyRenderer();
void Sys_DatabaseCompleted();
void Sys_WaitStartDatabase();
bool Sys_IsDatabaseReady();
void Sys_SyncDatabase();
void Sys_WakeDatabase();
void Sys_NotifyDatabase();
void Sys_DatabaseCompleted2();
bool Sys_IsDatabaseReady2();
void Sys_WakeDatabase2();
bool Sys_FinishRenderer();
int Sys_IsMainThreadReady();
void Sys_EndLoadThreadPriorities();
void Sys_WaitForMainThread();
void Sys_StopRenderer();
void Sys_StartRenderer();
bool Sys_IsRenderThread();
bool Sys_IsDatabaseThread();
bool Sys_IsMainThread();
#ifdef KISAK_SP
bool Sys_IsServerThread();
#endif
void Sys_SetValue(int valueIndex, void* data);
void* Sys_GetValue(int valueIndex);
void Sys_WaitForWorkerCmd();
void Sys_SetWorkerCmdEvent();
void Sys_ResetWorkerCmdEvent();
int Sys_WaitBackendEvent();
void Sys_SetUpdateSpotLightEffectEvent();
void Sys_ResetUpdateSpotLightEffectEvent();
void Sys_WaitUpdateNonDependentEffectsCompleted();
void Sys_SetUpdateNonDependentEffectsEvent();
void Sys_ResetUpdateNonDependentEffectsEvent();
void Sys_SuspendOtherThreads();
void Sys_ReleaseThreadOwnership();
char Sys_SpawnCinematicsThread(void(* function)(uint32_t));
bool Sys_WaitForCinematicsThreadOutstandingRequestEventTimeout(uint32_t timeoutMsec);
void Sys_SetCinematicsThreadOutstandingRequestEvent();
void Sys_ResetCinematicsThreadOutstandingRequestEvent();
bool Sys_WaitForCinematicsHostOutstandingRequestEventTimeout(uint32_t timeoutMsec);
void Sys_SetCinematicsHostOutstandingRequestEvent();
void Sys_ResetCinematicsHostOutstandingRequestEvent();

int Sys_IsRendererReady();
void Sys_BeginLoadThreadPriorities();

#ifdef KISAK_SP
int Sys_WaitStartServer(uint32_t timeout);
void Sys_InitServerEvents();
void Sys_ClientMessageReceived();
void Sys_ClearClientMessage();
int Sys_SpawnServerThread(void(*function)(uint32_t));
void Sys_WaitClientMessageReceived();
void Sys_ServerSnapshotCompleted();
bool Sys_WaitServerSnapshot();
void Sys_AllowSendClientMessages();
void Sys_DisallowSendClientMessages();
int Sys_CanSendClientMessages();
void Sys_ServerCompleted();
int Sys_ServerTimeout();
void Sys_WakeServer();
void Sys_SleepServer();
bool Sys_WaitServer();
void Sys_Sleep(uint32_t msec);
void Sys_SetServerTimeout(int timeout);
bool Sys_WaitForSaveHistoryDone();
int Sys_SpawnServerDemoThread(void(*function)(uint32_t));
void Sys_SetSaveHistoryEvent();
void Sys_WaitForSaveHistory();
void Sys_SetSaveHistoryDoneEvent();
#endif


enum WinThreadLock : __int32
{                                       // ...
    THREAD_LOCK_NONE = 0x0,
    THREAD_LOCK_MINIMAL = 0x1,
    THREAD_LOCK_ALL = 0x2,
};
void Win_SetThreadLock(WinThreadLock threadLock);
WinThreadLock Win_GetThreadLock();
void Win_UpdateThreadLock();


extern void *g_threadValues[THREAD_CONTEXT_COUNT][4];
extern DWORD threadId[THREAD_CONTEXT_COUNT];
extern HANDLE threadHandle[THREAD_CONTEXT_COUNT];
extern uint32_t s_affinityMaskForProcess;
extern uint32_t s_cpuCount;
extern uint32_t s_affinityMaskForCpu[4];