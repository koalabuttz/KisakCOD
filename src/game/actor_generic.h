#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

void Actor_Generic_Finish(actor_s *self, ai_state_t eNextState);
void Actor_Generic_Suspend(actor_s *self, ai_state_t eNextState);
bool Actor_Generic_Resume(actor_s *self, ai_state_t ePrevState);
void Actor_Generic_Pain(
    actor_s *self,
    gentity_s *attacker,
    int iDamage,
    const float *vPoint,
    const int iMod,
    const float *vDir,
    const hitLocation_t hitLoc);
void Actor_Generic_Touch(actor_s *self, gentity_s *pOther);
