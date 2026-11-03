#pragma once
#include "actor_events.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct AIEventListener
{
    int entIndex;
    unsigned int events;
};

void TRACK_actor_event_listener();
void Actor_EventListener_Init();
void Actor_EventListener_SetCount(int listenerCount);
int Actor_EventListener_GetCount();
int Actor_FindEventFromString(unsigned __int16 eventString);
void Actor_EventListener_Add(int entIndex, unsigned __int16 eventString);
void RemoveSwapWithLast(unsigned int listenerIndex);
void Actor_EventListener_Remove(int entIndex, unsigned __int16 eventString);
void Actor_EventListener_RemoveEntity(int entIndex);
int Actor_EventListener_Next(int index, int event, int teamFlags);
gentity_s *Actor_EventListener_GetEntity(unsigned int index);
void Actor_EventListener_NotifyToListener(
    gentity_s *listener,
    gentity_s *originator,
    ai_event_t event,
    const float *position);
void Actor_DumpEventListners();
int Actor_EventListener_First(int event, int teamFlags);

extern AIEventListener g_AIEVlisteners[32];