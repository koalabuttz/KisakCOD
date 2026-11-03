#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <bgame/bg_public.h>
#include "actor.h"

int Actor_SightTrace(actor_s *self, const float *start, const float *end, int passEntNum);
int Actor_CanSeePointFrom(
    actor_s *self,
    const float *vStart,
    const float *vEnd,
    double fMaxDistSqrd,
    int ignoreEntityNum);
int Actor_CanSeeEnemyViaClaimedNode(actor_s *self);
sentient_s *Actor_KnowAboutEnemy(actor_s *self, int hadPath);
int Actor_CanShootFrom(actor_s *self, const float *vTarget, const float *vFrom);
int compare_sentient_sort(unsigned int *pe1, unsigned int *pe2);
void Actor_UpdateLastKnownPos(actor_s *self, sentient_s *other);
void Actor_UpdateLastEnemySightPos(actor_s *self);
void Actor_UpdateEyeInformation(actor_s *self);
void Actor_GetEyePosition(actor_s *self, float *vEyePosOut);
void Actor_GetDebugEyePosition(actor_s *self, float *vEyePosOut);
void Actor_GetEyeDirection(actor_s *self, float *vEyeDir);
void Actor_GetEyeOffset(actor_s *self, float *vEyePosOut);
int Actor_GetMuzzleInfo(actor_s *self, float *vOrigin, float *vForward);
bool PointInFovAndRange(
    actor_s *self,
    const float *vEyePos,
    const float *vPoint,
    double fovDot,
    double fMaxDistSqrd);
int Actor_CanSeePointExInternal(
    actor_s *self,
    const float *vPoint,
    double fovDot,
    double fMaxDistSqrd,
    int ignoreEntityNum,
    float *vViewPos);
int Actor_CanSeePointEx(
    actor_s *self,
    const float *vPoint,
    double fovDot,
    double fMaxDistSqrd,
    int ignoreEntityNum);
void Actor_UpdateVisCache(actor_s *self, const gentity_s *ent, sentient_info_t *pInfo, bool bVisible);
int Actor_CanSeeEntityEx(actor_s *self, const gentity_s *ent, double fovDot, double fMaxDistSqrd);
int Actor_CanSeeSentientEx(
    actor_s *self,
    sentient_s *sentient,
    double fovDot,
    double fMaxDistSqrd,
    int iMaxLatency);
int Actor_CanShootEnemy(actor_s *self);
int Actor_CanSeePoint(actor_s *self, const float *vPoint);
int Actor_CanSeeEntityPoint(actor_s *self, const float *vPoint, const gentity_s *ent);
int Actor_CanSeeEntity(actor_s *self, const gentity_s *ent);
int Actor_CanSeeSentient(actor_s *self, sentient_s *sentient, int iMaxLatency);
int Actor_CanSeeEnemy(actor_s *self);
int Actor_CanSeeEnemyExtended(actor_s *self, int useClaimedNode);
void Actor_UpdateSight(actor_s *self);
