#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

void TRACK_actor_state();
void Actor_SetDefaultState(actor_s *actor);
int Actor_StartState(actor_s *self, ai_state_t eStartedState);
void Actor_FinishState(actor_s *self, ai_state_t eNextState);
void Actor_SuspendState(actor_s *self, ai_state_t eNextState);
int Actor_GetNextPopedState(actor_s *self);
int Actor_ResumeState(actor_s *self, ai_state_t ePrevState);
void Actor_StartDefaultState(actor_s *self);
void Actor_ResumePopedState(actor_s *self, ai_state_t state);
void Actor_ThinkStateTransitions(actor_s *self);
void Actor_SetSubState(actor_s *self, ai_substate_t eSubState);
int Actor_IsStateOnStack(const actor_s *self, ai_state_t eState);
int Actor_PendingTransitionTo(actor_s *self, ai_state_t eState);
void Actor_SimplifyStateTransitions(actor_s *self);
int Actor_AllowedToPushState(actor_s *self, int eState);
void Actor_OnStateChange(actor_s *self);
void Actor_SetState(actor_s *self, ai_state_t eState);
void Actor_PopState(actor_s *self);
void Actor_ForceState(actor_s *self, ai_state_t eState);
void Actor_PrepareToPushState(actor_s *self, int eState);
int Actor_PushState(actor_s *self, ai_state_t eState);
