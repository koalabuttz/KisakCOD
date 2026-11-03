#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

bool Actor_ScriptedAnim_Start(actor_s *self, ai_state_t ePrevState);
void Actor_ScriptedAnim_Finish(actor_s *self, ai_state_t eNextState);
actor_think_result_t Actor_ScriptedAnim_Think(actor_s *self);
bool Actor_CustomAnim_Start(actor_s *self, ai_state_t ePrevState);
actor_think_result_t Actor_CustomAnim_Think(actor_s *self);
