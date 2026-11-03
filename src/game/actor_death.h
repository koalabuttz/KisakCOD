#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

bool Actor_Death_Start(actor_s *self, ai_state_t ePrevState);
void Actor_Death_Cleanup(actor_s *self);
actor_think_result_t Actor_Death_Think(actor_s *self);
