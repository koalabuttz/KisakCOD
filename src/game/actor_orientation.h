#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

void Actor_SetDesiredLookAngles(ai_orient_t *pOrient, double fPitch, double fYaw);
void Actor_SetDesiredBodyAngle(ai_orient_t *pOrient, double fAngle);
void Actor_SetDesiredAngles(ai_orient_t *pOrient, double fPitch, double fYaw);
void Actor_SetLookAngles(actor_s *self, double fPitch, double fYaw);
void Actor_SetBodyAngle(actor_s *self, double fAngle);
void Actor_ChangeAngles(actor_s *self, double fPitch, double fYaw);
void Actor_UpdateLookAngles(actor_s *self);
void Actor_UpdateBodyAngle(actor_s *self);
void Actor_FaceVector(ai_orient_t *pOrient, const float *v);
void Actor_FaceMotion(actor_s *self, ai_orient_t *pOrient);
void Actor_SetAnglesToLikelyEnemyPath(actor_s *self);
const pathnode_t *Actor_GetAnglesToLikelyEnemyPath(actor_s *self);
void Actor_FaceLikelyEnemyPath(actor_s *self, ai_orient_t *pOrient);
void Actor_FaceEnemy(actor_s *self, ai_orient_t *pOrient);
int Actor_FaceGoodShootPos(actor_s *self, ai_orient_t *pOrient);
void Actor_FaceEnemyOrMotion(actor_s *self, ai_orient_t *pOrient);
void Actor_FaceEnemyOrMotionSidestep(actor_s *self, ai_orient_t *pOrient);
void Actor_DecideOrientation(actor_s *self);
void Actor_SetOrientMode(actor_s *self, ai_orient_mode_t eMode);
void Actor_ClearScriptOrient(actor_s *self);
