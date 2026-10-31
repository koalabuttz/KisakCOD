#pragma once

struct sysEvent_t;
// Consume a queued engine event without polling console/window backends.
bool Sys_PollQueuedEvent(sysEvent_t *result);
void Sys_ShutdownEvents();
// Focus/cancel boundary for client hosts. Preserve non-input events and their
// payload ownership; callers must also reset the consumer's held-key state.
void Sys_DiscardQueuedInputEvents();
