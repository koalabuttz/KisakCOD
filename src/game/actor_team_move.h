#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include "actor.h"
#include "sentient.h"

enum ai_teammove_t : __int32
{
    AI_TEAMMOVE_TRAVEL = 0x0,
    AI_TEAMMOVE_WAIT = 0x1,
    AI_TEAMMOVE_SLOW_DOWN = 0x2,
};

struct team_move_context_t
{
    actor_s *self;
    float vVelSelf[2];
    float vOrgSelf[3];
    float vVelDirSelf[2];
    float fVelSelfSqrd;
    float fDeltaCorrection;
    float fIntervalSqrd;
    float fWalkIntervalSqrd;
    float fMaxIntervalSqrd;
    int bFailedLookahead;
    float fDodgePosDeltaLengthSqrd;
    int dodgeEntities[33];
    int dodgeEntityCount;
    sentient_s *pDodgeOther;
    bool bPileUp;
    float fSlowDownPosDeltaLengthSqrd;
    sentient_s *pSlowDownOther;
    float vVelSlowDownOther[2];
};

struct team_move_other_context_t
{
    sentient_s *other;
    float vOrgOther[3];
    float vVelOther[2];
    float vDelta[2];
    float vPerp[2];
    float fPosDeltaLengthSqrd;
    float fScale;
};


void Actor_TeamMoveBlocked(actor_s *self);
void Actor_TeamMoveBlockedClear(actor_s *self);
bool Actor_TeamMoveCheckWaitTimer(actor_s *self, ai_teammove_t *result);
bool Actor_TeamMoveNeedToCheckWait(unsigned __int8 moveMode, path_t *pPath);
bool Actor_IsEnemy(actor_s *self, sentient_s *other);
void Actor_CalcInterval(
    actor_s *self,
    bool bUseInterval,
    float *fIntervalSqrdOut,
    float *fWalkIntervalSqrdOut);
int Actor_TeamMoveCalcMovementDir(team_move_context_t *context, ai_teammove_t *result);
float Actor_TeamMoveDeltaCorrection(actor_s *self, double fVelSelfSqrd);
void Actor_AddToList(int *dodgeEntities, int *dodgeEntityCount, int arraysz, actor_s *pOtherActor);
void Actor_TeamMoveSetDodge(team_move_context_t *context, team_move_other_context_t *context_other);
int Actor_TeamMoveShouldTryDodgeSentient(
    team_move_context_t *context,
    team_move_other_context_t *context_other);
int Actor_TeamMoveTryDodge(team_move_context_t *context, team_move_other_context_t *context_other);
int Actor_TeamMoveConsiderSlowDown(team_move_context_t *context, ai_teammove_t *eResult);
ai_teammove_t Actor_TeamMoveNoDodge(team_move_context_t *context, ai_teammove_t eResult);
void Actor_TeamMoveInitializeContext(
    actor_s *self,
    bool bUseInterval,
    bool bAllowGoalPileUp,
    team_move_context_t *context);
int Actor_TeamMoveTrimPath(path_t *pPath, const team_move_context_t *context);
void Actor_TeamMoveTooCloseMoveAway(const actor_s *self, int mask, team_move_context_t *context);
int Actor_TeamMoveCheckPileup(actor_s *self, actor_s *pOtherActor);
void Actor_DodgeDebug(
    actor_s *self,
    actor_s *otherActor,
    const char *debugString,
    int a4,
    int a5,
    int a6,
    int a7);
ai_teammove_t Actor_GetTeamMoveStatus(actor_s *self, bool bUseInterval, bool bAllowGoalPileUp);
void Actor_MoveAlongPathWithTeam(actor_s *self, bool bRun, bool bUseInterval, bool bAllowGoalPileUp);
