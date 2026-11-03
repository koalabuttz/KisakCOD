#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

bool Actor_Dog_Exposed_Start(actor_s *self, ai_state_t ePrevState);
void Actor_Dog_Exposed_Finish(actor_s *self, ai_state_t eNextState);
void Actor_Dog_Exposed_Suspend(actor_s *self, ai_state_t eNextState);
int Actor_Dog_IsInSyncedMelee(actor_s *self, sentient_s *enemy);
void Actor_Dog_Attack(actor_s *self);
void Actor_FindPathToGoalNearestNode(actor_s *self);
int Actor_SetMeleeAttackSpot(actor_s *self, const float *enemyPosition, float *attackPosition);
void Actor_UpdateMeleeGoalPos(actor_s *self, float *goalPos);
int Actor_Dog_IsAttackScriptRunning(actor_s *self);
float Actor_Dog_GetEnemyPos(actor_s *self, sentient_s *enemy, float *enemyPos);
bool Actor_Dog_IsEnemyInAttackRange(actor_s *self, sentient_s *enemy, int *goalPosSet);
actor_think_result_t Actor_Dog_Exposed_Think(actor_s *self);
