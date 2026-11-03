#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

bool Actor_Negotiation_Start(actor_s *pSelf, ai_state_t ePrevState);
actor_think_result_t Actor_Negotiation_Think(actor_s *pSelf);
