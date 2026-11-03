#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

enum ActorGrenadeReevaluateMode : __int32
{
    REEVALUATE_ATTEMPT_RETURN = 0x0,
    REEVALUATE_NO_RETURN = 0x1,
};

void TRACK_actor_grenade();
void SP_info_grenade_hint(gentity_s *ent);
bool Actor_Grenade_IsValidTrajectory(
    actor_s *self,
    const float *vFrom,
    float *vVelocity,
    const float *vGoal);
void Actor_Grenade_GetTossFromPosition(
    actor_s *self,
    const float *vStandPos,
    const float *vOffset,
    float *vFrom);
void Actor_Grenade_GetTossPositions(
    const float *vFrom,
    float *vTargetPos,
    float *vLand,
    unsigned int grenadeWPID);
int Actor_Grenade_GetTossPositionsFromHints(
    const float *vFrom,
    const float *vTargetPos,
    const unsigned int method,
    float *vLand);
bool Actor_Grenade_CheckMaximumEnergyToss(
    actor_s *self,
    float *vFrom,
    float *vLand,
    int bLob,
    float *vVelOut);
bool Actor_Grenade_CheckInfiniteEnergyToss(actor_s *self, float *vFrom, float *vLand, float *vVelOut);
bool Actor_Grenade_CheckMinimumEnergyToss(actor_s *self, float *vFrom, float *vLand, float *vVelOut);
int Actor_Grenade_CheckGrenadeHintToss(actor_s *self, float *vFrom, float *vLand, float *vVelOut);
int compare_desperate_hints(float *pe0, float *pe1);
int Actor_Grenade_CheckDesperateToss(actor_s *self, float *vFrom, float *vVelOut);
bool Actor_GrenadeLauncher_CheckPos(
    actor_s *self,
    const float *vStandPos,
    const float *vOffset,
    const float *vTargetPos,
    float speed,
    float *vPosOut,
    float *vVelOut);
int Actor_Grenade_IsSafeTarget(actor_s *self, const float *vTargetPos, unsigned int iWeapID);
void Actor_PredictGrenadeLandPos(gentity_s *pGrenade);
bool Actor_Grenade_IsPointSafe(actor_s *self, const float *vPoint);
float Actor_Grenade_EscapePlane(actor_s *self, float *normal);
void Actor_Grenade_GetPickupPos(actor_s *self, const float *enemyPos, float *vGrenadePickupPos);
bool Actor_Grenade_ShouldIgnore(actor_s *self, gentity_s *grenade);
int Actor_IsAwareOfGrenade(actor_s *self);
void Actor_GrenadePing(actor_s *self, gentity_s *pGrenade);
void Actor_DissociateGrenade(gentity_s *pGrenade);
void Actor_Grenade_Attach(actor_s *self);
void Actor_Grenade_Detach(actor_s *self);
int Actor_Grenade_InActorHands(gentity_s *grenade);
bool Actor_Grenade_Resume(actor_s *self, ai_state_t ePrevState);
void Actor_Grenade_Cower(actor_s *self);
void Actor_Grenade_Combat(actor_s *self);
void Actor_Grenade_CoverAttack(actor_s *self);
void Actor_Grenade_Flee(actor_s *self);
void Actor_Grenade_TakeCover(actor_s *self);
actor_think_result_t Actor_Grenade_ThrowBack(actor_s *self);
void G_DrawGrenadeHints();
bool Actor_Grenade_CheckTossPos(
    actor_s *self,
    float *vStandPos,
    float *vOffset,
    float *vTargetPos,
    unsigned int method,
    float *vPosOut,
    float *vVelOut,
    double randomRange,
    int bRechecking);
bool Actor_Grenade_AttemptReturnTo(
    actor_s *self,
    float *vFrom,
    float *vEnemyPos,
    float *vLand,
    float *vVelocity);
int Actor_Grenade_AttemptReturn(actor_s *self);
void Actor_Grenade_DropIfHeld(actor_s *self);
void Actor_Grenade_Finish(actor_s *self, ai_state_t eNextState);
void Actor_Grenade_Suspend(actor_s *self, ai_state_t eNextState);
bool Actor_Grenade_CheckToss(
    actor_s *self,
    float *vStandPos,
    float *vOffset,
    unsigned int method,
    float *vPosOut,
    float *vVelOut,
    double randomRange,
    int bRechecking);
void Actor_Grenade_AttemptEscape(actor_s *self, int bForceAbortPath);
void Actor_Grenade_DecideResponse(actor_s *self);
int Actor_Grenade_ReevaluateResponse(actor_s *self, ActorGrenadeReevaluateMode reevaluateMode);
void Actor_GrenadeBounced(gentity_s *pGrenade, gentity_s *pHitEnt);
bool Actor_Grenade_Start(actor_s *self, ai_state_t ePrevState);
actor_think_result_t Actor_Grenade_Acquire(actor_s *self);
actor_think_result_t Actor_Grenade_Think(actor_s *self);
