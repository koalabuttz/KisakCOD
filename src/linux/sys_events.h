#pragma once

#include <cstdint>

struct sysEvent_t;
// Consume a queued engine event without polling console/window backends.
bool Sys_PollQueuedEvent(sysEvent_t *result);
void Sys_ShutdownEvents();
// Queue one pointer motion sample. Motion travels in the same queue as keys and
// text, so a click keeps its arrival order against a later motion sample. The
// absolute position (evValue3/evValue4) and the delta (evValue/evValue2) stay in
// one record because a consumer that applied them separately could see the
// position of one sample and the delta of another. Consecutive samples collapse
// into the record already queued, and a full queue reclaims the oldest queued
// sample before it evicts anything, so held motion cannot crowd out a transition.
void Sys_QueMouseEvent(uint32_t time, int x, int y, int dx, int dy);
// Queue one pointer boundary (evValue!=0 means entered). The consumer must see
// it in order with the motion and keys around it; the platform cursor changes
// the same boundary causes stay window actions taken at poll time.
void Sys_QuePointerPresence(uint32_t time, int inside);
// Focus/cancel boundary for client hosts. Drops queued key, text and motion
// input, preserves other events including pointer boundaries and their payload
// ownership; callers must also reset the consumer's held-key state.
void Sys_DiscardQueuedInputEvents();
