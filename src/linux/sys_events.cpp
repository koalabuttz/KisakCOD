// Shared engine event queue. Platform backends feed Sys_QueEvent; the existing
// dedicated event loop and optional window host consume the same representation.
#include "sys_events.h"
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <universal/com_memory.h>

sysEvent_t eventQue[0x100];
int eventHead;
int eventTail;

void Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);

    sysEvent_t *ev = &eventQue[(unsigned char)eventHead];

    if (eventHead - eventTail >= 256)
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "Sys_QueEvent: overflow\n");
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 10);
        ++eventTail;
    }

    ++eventHead;

    if (!time)
        time = Sys_Milliseconds();

    ev->evTime = (int)time;
    ev->evType = type;
    ev->evValue = value;
    ev->evValue2 = value2;
    ev->evPtrLength = ptrLength;
    ev->evPtr = ptr;

    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

void Sys_ShutdownEvents()
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);

    while (eventHead > eventTail)
    {
        sysEvent_t *ev = &eventQue[(unsigned char)eventTail++];
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 10);
    }

    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

bool Sys_PollQueuedEvent(sysEvent_t *result)
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    if (eventHead > eventTail)
    {
        *result = eventQue[static_cast<unsigned char>(eventTail++)];
        Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
        return true;
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    return false;
}

void Sys_DiscardQueuedInputEvents()
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    int write = eventTail;
    for (int read = eventTail; read < eventHead; ++read)
    {
        sysEvent_t event = eventQue[static_cast<unsigned char>(read)];
        if (event.evType == SE_KEY || event.evType == SE_CHAR)
        {
            if (event.evPtr) Z_Free(event.evPtr, 10);
        }
        else
        {
            eventQue[static_cast<unsigned char>(write++)] = event;
        }
    }
    eventHead = write;
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}
