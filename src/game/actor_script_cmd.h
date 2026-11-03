#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include "actor.h"
#include <script/scr_variable.h>

// LWSS: not sure this .h is supposed to exist.

actor_s *Actor_Get(scr_entref_t entref);
void Actor_SetScriptGoalPos(actor_s *self, const float *vGoalPos, pathnode_t *node);
void ActorCmd_StartScriptedAnim(scr_entref_t entref);
void Actor_StartArrivalState(actor_s *self, ai_state_t newState);
void ActorCmd_StartCoverArrival(scr_entref_t entref);
void ActorCmd_StartTraverseArrival(scr_entref_t entref);
void ActorCmd_CheckCoverExitPosWithPath(scr_entref_t entref);
void ActorCmd_Shoot(scr_entref_t entref);
void ActorCmd_ShootBlank(scr_entref_t entref);
void ActorCmd_Melee(scr_entref_t entref);
void ActorCmd_UpdatePlayerSightAccuracy(scr_entref_t entref);
void ActorCmd_FindCoverNode(scr_entref_t entref);
void ActorCmd_FindBestCoverNode(scr_entref_t entref);
void ActorCmd_GetCoverNode(scr_entref_t entref);
void ActorCmd_UseCoverNode(scr_entref_t entref);
void ActorCmd_ReacquireStep(scr_entref_t entref);
void ActorCmd_FindReacquireNode(scr_entref_t entref);
void ActorCmd_GetReacquireNode(scr_entref_t entref);
void ActorCmd_UseReacquireNode(scr_entref_t entref);
void ActorCmd_FindReacquireDirectPath(scr_entref_t entref);
void ActorCmd_FindReacquireProximatePath(scr_entref_t entref);
void ActorCmd_TrimPathToAttack(scr_entref_t entref);
void ActorCmd_ReacquireMove(scr_entref_t entref);
void ActorCmd_FlagEnemyUnattackable(scr_entref_t entref);
void ActorCmd_SetAimAnims(scr_entref_t entref);
void ActorCmd_AimAtPos(scr_entref_t entref);
void ActorCmd_EnterProne(scr_entref_t entref);
void ActorCmd_ExitProne(scr_entref_t entref);
void ActorCmd_SetProneAnimNodes(scr_entref_t entref);
void ActorCmd_UpdateProne(scr_entref_t entref);
void ActorCmd_ClearPitchOrient(scr_entref_t entref);
void ActorCmd_SetLookAtAnimNodes(scr_entref_t entref);
void ActorCmd_SetLookAt(scr_entref_t entref);
void ActorCmd_SetLookAtYawLimits(scr_entref_t entref);
void ActorCmd_StopLookAt(scr_entref_t entref);
void ActorCmd_CanShoot(scr_entref_t entref);
void ActorCmd_CanSee(scr_entref_t entref);
void ActorCmd_DropWeapon(scr_entref_t entref);
void ActorCmd_MayMoveToPoint(scr_entref_t entref);
void ActorCmd_MayMoveFromPointToPoint(scr_entref_t entref);
void ActorCmd_Teleport(scr_entref_t entref);
void ActorCmd_WithinApproxPathDist(scr_entref_t entref);
void ActorCmd_IsPathDirect(scr_entref_t entref);
void ActorCmd_AllowedStances(scr_entref_t entref);
void ActorCmd_IsStanceAllowed(scr_entref_t entref);
void ActorCmd_IsSuppressionWaiting(scr_entref_t entref);
void ActorCmd_IsSuppressed(scr_entref_t entref);
void ActorCmd_IsMoveSuppressed(scr_entref_t entref);
void ActorCmd_CheckGrenadeThrow(scr_entref_t entref);
void ActorCmd_CheckGrenadeThrowPos(scr_entref_t entref);
void ActorCmd_ThrowGrenade(scr_entref_t entref);
bool Actor_CheckGrenadeLaunch(actor_s *self, const float *vStartPos, const float *vOffset);
void ActorCmd_CheckGrenadeLaunch(scr_entref_t entref);
void ActorCmd_CheckGrenadeLaunchPos(scr_entref_t entref);
void ActorCmd_FireGrenadeLauncher(scr_entref_t entref);
void ActorCmd_PickUpGrenade(scr_entref_t entref);
void ActorCmd_UseTurret(scr_entref_t entref);
void ActorCmd_StopUseTurret(scr_entref_t entref);
void ActorCmd_CanUseTurret(scr_entref_t entref);
void ActorCmd_TraverseMode(scr_entref_t entref);
void ActorCmd_AnimMode(scr_entref_t entref);
void ActorCmd_OrientMode(scr_entref_t entref);
void ActorCmd_GetMotionAngle(scr_entref_t entref);
void ActorCmd_GetAnglesToLikelyEnemyPath(scr_entref_t entref);
void ActorCmd_SetTurretAnim(scr_entref_t entref);
void ActorCmd_GetTurret(scr_entref_t entref);
void ActorCmd_BeginPrediction(scr_entref_t entref);
void ActorCmd_EndPrediction(scr_entref_t entref);
void ActorCmd_LerpPosition(scr_entref_t entref);
void ActorCmd_PredictOriginAndAngles(scr_entref_t entref);
void ActorCmd_PredictAnim(scr_entref_t entref);
void ActorCmd_GetHitEntType(scr_entref_t entref);
void ActorCmd_GetHitYaw(scr_entref_t entref);
void ActorCmd_GetGroundEntType(scr_entref_t entref);
void ActorCmd_IsDeflected(scr_entref_t entref);
void ActorCmd_trackScriptState(scr_entref_t entref);
void ActorCmd_DumpHistory(scr_entref_t entref);
void ScrCmd_animcustom(scr_entref_t entref);
void ScrCmd_CanAttackEnemyNode(scr_entref_t entref);
void ScrCmd_GetNegotiationStartNode(scr_entref_t entref);
void ScrCmd_GetNegotiationEndNode(scr_entref_t entref);
void ActorCmd_CheckProne(scr_entref_t entref);
void ActorCmd_PushPlayer(scr_entref_t entref);
void ActorCmd_SetGoalNode(scr_entref_t entref);
void ActorCmd_SetGoalPos(scr_entref_t entref);
void ActorCmd_SetGoalEntity(scr_entref_t entref);
void ActorCmd_SetGoalVolume(scr_entref_t entref);
void ActorCmd_GetGoalVolume(scr_entref_t entref);
void ActorCmd_ClearGoalVolume(scr_entref_t entref);
void ActorCmd_SetFixedNodeSafeVolume(scr_entref_t entref);
void ActorCmd_GetFixedNodeSafeVolume(scr_entref_t entref);
void ActorCmd_ClearFixedNodeSafeVolume(scr_entref_t entref);
void ActorCmd_IsInGoal(scr_entref_t entref);
void ActorCmd_SetOverrideRunToPos(scr_entref_t entref);
void ActorCmd_NearNode(scr_entref_t entref);
void ActorCmd_ClearEnemy(scr_entref_t entref);
void ActorCmd_SetEntityTarget(scr_entref_t entref);
void ActorCmd_ClearEntityTarget(scr_entref_t entref);
void ActorCmd_SetPotentialThreat(scr_entref_t entref);
void ActorCmd_ClearPotentialThreat(scr_entref_t entref);
void ActorCmd_SetFlashBanged(scr_entref_t entref);
void ActorCmd_SetFlashbangImmunity(scr_entref_t entref);
void ActorCmd_GetFlashBangedStrength(scr_entref_t entref);
void ActorCmd_SetEngagementMinDist(scr_entref_t entref);
void ActorCmd_SetEngagementMaxDist(scr_entref_t entref);
void ActorCmd_IsKnownEnemyInRadius(scr_entref_t entref);
void ActorCmd_IsKnownEnemyInVolume(scr_entref_t entref);
void ActorCmd_SetTalkToSpecies(scr_entref_t entref);


void(*Actor_GetMethod(const char **pName))(scr_entref_t);
