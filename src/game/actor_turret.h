#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct actor_s;
struct gentity_s;

void Actor_Turret_Touch(actor_s *self, gentity_s *pOther);
int Actor_IsUsingTurret(actor_s *self);
int Actor_UseTurret(actor_s *self, gentity_s *pTurret);
bool Actor_Turret_Start(actor_s *self, ai_state_t ePrevState);
void Actor_DetachTurret(actor_s *self);
void Actor_Turret_Finish(actor_s *self, ai_state_t eNextState);
void Actor_Turret_Suspend(actor_s *self, ai_state_t eNextState);
void Actor_StopUseTurret(actor_s *self);
actor_think_result_t Actor_Turret_PostThink(actor_s *self);
actor_think_result_t Actor_Turret_Think(actor_s *self);
void Actor_Turret_Pain(
    actor_s *self,
    gentity_s *pAttacker,
    int iDamage,
    const float *vPoint,
    const int iMod,
    const float *vDir,
    const hitLocation_t hitLoc);
