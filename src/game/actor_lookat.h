#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include "actor.h"

void Actor_InitLookAt(actor_s *self);
void Actor_SetLookAtAnimNodes(
    actor_s *self,
    unsigned __int16 animStraight,
    unsigned __int16 animLeft,
    unsigned __int16 animRight);
void Actor_SetLookAt(actor_s *self, float *vPosition, double fTurnAccel);
float Actor_CurrentLookAtAnimYawMax(actor_s *self);
float Actor_CurrentLookAtYawMax(actor_s *self);
void Actor_SetLookAtYawLimits(actor_s *self, double fAnimYawLimit, double fYawLimit, double fBlendTime);
void Actor_StopLookAt(actor_s *self, double fTurnAccel);
void Actor_UpdateLookAt(actor_s *self);
