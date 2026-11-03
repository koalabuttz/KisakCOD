#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

bool Actor_CheckCoverLeave(actor_s *self, const float *exitPos);
int Actor_CheckCoverApproach(actor_s *self);
void Actor_CoverApproachNotify(actor_s *self);
bool Actor_CoverArrival_Start(actor_s *self, ai_state_t ePrevState);
bool Actor_CoverArrival_Resume(actor_s *self, ai_state_t ePrevState);
actor_think_result_t Actor_CoverArrival_Think(actor_s *self);
