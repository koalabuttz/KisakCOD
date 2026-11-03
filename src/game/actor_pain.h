#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

bool Actor_InPain(const actor_s *self);
bool Actor_Pain_Start(actor_s *self, ai_state_t ePrevState);
void Actor_Pain_Finish(actor_s *self, ai_state_t eNextState);
actor_think_result_t Actor_Pain_Think(actor_s *self);
