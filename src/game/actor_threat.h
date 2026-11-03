#pragma once
#include "actor.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

enum ThreatDebugStringCategory : __int32
{
    TDS_TOTAL_THREAT = 0x0,
    TDS_FLASHED = 0x1,
    TDS_SUPPRESSED = 0x2,
    TDS_THREATBIAS = 0x3,
    TDS_THREATBIAS_GROUP = 0x4,
    TDS_ATTACKER_COUNT = 0x5,
    TDS_CUR_ENEMY_BONUS = 0x6,
    TDS_AWARENESS = 0x7,
    TDS_DIST_THREAT = 0x8,
    TDS_SCARINESS_THREAT = 0x9,
    NUM_TDS = 0xA,
};

struct threat_bias_t
{
    unsigned __int16 groupName[16];
    int threatTable[16][16];
    int threatGroupCount;
};

void TRACK_actor_threat();
void Actor_InitThreatBiasGroups();
void Actor_ClearThreatBiasGroups();
int Actor_FindThreatBiasGroupIndex(unsigned int name);
void Actor_CreateThreatBiasGroup(unsigned int name);
void Actor_SetThreatBiasEntireGroup(int group, int threatBias);
void Actor_SetThreatBias(int groupSelf, int groupEnemy, int threatBias);
void Actor_SetIgnoreMeGroup(int groupSelf, int groupIgnoreMe);
int Actor_GetThreatBias(int groupSelf, int groupEnemy);
void Actor_FlagEnemyUnattackable(actor_s *self);
int Actor_CaresAboutInfo(actor_s *self, sentient_s *pOther);
int DebugThreatInfoDuration();
void DebugResetThreatStrings(const actor_s *self);
void DebugSetThreatString(ThreatDebugStringCategory category, int threat);
void DebugSetThreatStringFromString(ThreatDebugStringCategory category, const char *string);
void DebugThreatStringAll(const actor_s *self, sentient_s *enemy, int threat);
void DebugThreatStringSimple(const actor_s *self, gentity_s *enemy, const char *string, const float *color);
void DebugThreatNodes(
    const actor_s *self,
    sentient_s *enemy,
    pathnode_t *selfNode,
    pathnode_t *enemyNode,
    const float *color);
int Actor_ThreatFromScariness(double fScariness);
int Actor_ThreatFromDistance(float fDistance);
bool Actor_IsFullyAware(actor_s *self, sentient_s *enemy, int isCurrentEnemy);
int Actor_ThreatFromVisibilityAndAwareness(int isVisible, int isFullyAware, int friendlyTimingOut);
int Actor_ThreatFromAttackerCount(actor_s *self, sentient_s *enemy, int isCurrentEnemy);
int Actor_ThreatBonusForCurrentEnemy(int isCurrentEnemy, int isFullyAware, int friendlyTimingOut, int isPlayer, int isDamaged);
int Actor_ThreatCoveringFire(actor_s *self, sentient_s *enemy);
int Actor_ThreatFlashed(sentient_s *enemy);
int Actor_UpdateSingleThreat(actor_s *self, sentient_s *enemy);
void Actor_InitThreatUpdateInterval(actor_s *self);
void Actor_IncrementThreatTime(actor_s *self);
void Actor_CanAttackAll(actor_s *self);
void Actor_SetPotentialThreat(potential_threat_t *self, float yaw);
void Actor_ClearPotentialThreat(potential_threat_t *self);
bool Actor_GetPotentialThreat(potential_threat_t *self, float *potentialThreatDir);
void Actor_PotentialThreat_Debug(actor_s *self);
int Actor_CheckIgnore(sentient_s *self, sentient_s *enemy);
void Actor_UpdateThreat(actor_s *self);


extern threat_bias_t g_threatBias;