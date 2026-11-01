// Shared engine event queue. Platform backends feed Sys_QueEvent; the existing
// dedicated event loop and optional window host consume the same representation.
#include "sys_events.h"
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <universal/com_memory.h>
#include <algorithm>
#include <cstdint>

sysEvent_t eventQue[0x100];
int eventHead;
int eventTail;

// Reclaim the oldest queued motion sample so a full queue can admit one more
// record without dropping a key or text transition. Everything else keeps the
// order it was queued in; the removed sample carried no payload.
static bool ReclaimOldestMotionLocked()
{
    int oldest = -1;
    for (int index = eventTail; index < eventHead; ++index)
    {
        if (eventQue[(unsigned char)index].evType == SE_MOUSE)
        {
            oldest = index;
            break;
        }
    }

    if (oldest < 0)
        return false;

    for (int index = oldest; index > eventTail; --index)
        eventQue[(unsigned char)index] = eventQue[(unsigned char)(index - 1)];

    // The slot that leaves the live range must not keep a copied payload
    // pointer: the ring would free it again when it wraps onto that slot.
    eventQue[(unsigned char)eventTail] = sysEvent_t{};
    ++eventTail;
    return true;
}

static sysEvent_t *AppendRecordLocked(uint32_t time, sysEventType_t type, int value, int value2, int value3,
                                      int value4, int ptrLength, void *ptr)
{
    sysEvent_t *ev = &eventQue[(unsigned char)eventHead];

    if (eventHead - eventTail >= 256)
    {
        // Under pressure, admission reclaims the oldest queued motion sample
        // first, whatever this record's type is. A lost motion sample is
        // recoverable -- the next sample carries the current position and its own
        // delta -- while a lost key release strands a held key. Only when there
        // is no motion to reclaim does a record actually get evicted, and a
        // motion that cannot reclaim anything yields before evicting a
        // transition.
        if (!ReclaimOldestMotionLocked())
        {
            if (type == SE_MOUSE)
                return nullptr;

            Com_Printf(CON_CHANNEL_SYSTEM, "Sys_QueEvent: overflow\n");
            if (ev->evPtr)
                Z_Free((char *)ev->evPtr, 10);
            ++eventTail;
        }
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
    ev->evValue3 = value3;
    ev->evValue4 = value4;
    return ev;
}

static int AddedDelta(int current, int sample)
{
    // Coalescing sums deltas, so bound the sum: a consumer adds the record into
    // an int and must not overflow however long a run of samples becomes.
    constexpr int64_t limit = 1 << 20;
    return (int)std::clamp<int64_t>(int64_t(current) + int64_t(sample), -limit, limit);
}

void Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    AppendRecordLocked(time, type, value, value2, 0, 0, ptrLength, ptr);
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

void Sys_QueMouseEvent(uint32_t time, int x, int y, int dx, int dy)
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);

    // Consecutive samples are one uninterrupted run of pointer motion: the last
    // position plus the summed delta is what applying them one by one produces.
    // A key, click or pointer boundary between two samples still separates them,
    // which is what keeps a click ahead of the motion that arrived after it.
    if (eventHead > eventTail)
    {
        sysEvent_t *last = &eventQue[(unsigned char)(eventHead - 1)];
        if (last->evType == SE_MOUSE)
        {
            last->evValue = AddedDelta(last->evValue, dx);
            last->evValue2 = AddedDelta(last->evValue2, dy);
            last->evValue3 = x;
            last->evValue4 = y;
            Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
            return;
        }
    }

    AppendRecordLocked(time, SE_MOUSE, dx, dy, x, y, 0, nullptr);
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

void Sys_QuePointerPresence(uint32_t time, int inside)
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    AppendRecordLocked(time, SE_POINTER, inside ? 1 : 0, 0, 0, 0, 0, nullptr);
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
        // Motion is queued input as well: a sample left behind by a lost focus
        // would be applied against the pointer state of the next session.
        if (event.evType == SE_KEY || event.evType == SE_CHAR || event.evType == SE_MOUSE)
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
