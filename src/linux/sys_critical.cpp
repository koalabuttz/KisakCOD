#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <universal/assertive.h>
#include <universal/profile.h>

#include "sys_critical.h"
#include "sys_posix.h"

#include <sched.h>

// Referenced by <win32/win_local.h>. The engine never touches it directly; the
// real locks are the recursive POSIX mutexes in sys_posix.cpp.
std::mutex s_criticalSections[CRITSECT_COUNT];

namespace
{
    bool s_critInitialized = false;
}

void Sys_InitializeCriticalSections()
{
    if (s_critInitialized)
        return;

    Posix_InitCriticalSections(CRITSECT_COUNT);
    s_critInitialized = true;
}

void Sys_EnterCriticalSection(int critSect)
{
    PROF_SCOPED("Sys_EnterCriticalSection");

    iassert(critSect >= 0 && critSect < CRITSECT_COUNT);

    // Sections are recursive: Com_Printf takes CRITSECT_CONSOLE and is called
    // from code that already holds it.
    Posix_LockCriticalSection(critSect);
}

void Sys_LeaveCriticalSection(int critSect)
{
    iassert(critSect >= 0 && critSect < CRITSECT_COUNT);

    Posix_UnlockCriticalSection(critSect);
}

int Posix_CriticalSectionDepthForTest(int critSect)
{
    return Posix_CriticalSectionDepth(critSect);
}

// ---------------------------------------------------------------------------
// Spin-lock style helpers used by the renderer paths.
// ---------------------------------------------------------------------------

void Sys_LockWrite(FastCriticalSection *critSect)
{
    while (true)
    {
        if (__atomic_load_n(&critSect->readCount, __ATOMIC_ACQUIRE) == 0)
        {
            if (__atomic_add_fetch(&critSect->writeCount, 1, __ATOMIC_ACQ_REL) == 1
                && __atomic_load_n(&critSect->readCount, __ATOMIC_ACQUIRE) == 0)
            {
                break;
            }
            __atomic_sub_fetch(&critSect->writeCount, 1, __ATOMIC_ACQ_REL);
        }
        sched_yield();
    }
}

void Sys_UnlockWrite(FastCriticalSection *critSect)
{
    iassert(critSect->writeCount > 0);
    __atomic_sub_fetch(&critSect->writeCount, 1, __ATOMIC_ACQ_REL);
}

int Sys_InterlockedIncrement(uint *addend)
{
    return (int)__atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

int Sys_InterlockedDecrement(uint *addend)
{
    return (int)__atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}
