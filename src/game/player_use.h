#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct gentity_s;
struct gclient_s;

struct useList_t
{
    gentity_s *ent;
    float score;
};

void Player_UseEntity(gentity_s *playerEnt, gentity_s *useEnt);
int Player_ActivateCmd(gentity_s *ent);
void Player_ActivateHoldCmd(gentity_s *ent);
void Player_UpdateActivate(gentity_s *ent);
int compare_use(float *pe1, float *pe2);
int Player_GetUseList(gentity_s *ent, useList_t *useList, int prevHintEntIndex);
void G_UpdateFriendlyOverlay(gentity_s *ent);
int Player_GetItemCursorHint(const gclient_s *client, const gentity_s *traceEnt);
void Player_SetTurretDropHint(gentity_s *ent);
void Player_UpdateCursorHints(gentity_s *ent);
gentity_s *Player_UpdateLookAtEntityTrace(
    trace_t *trace,
    const float *start,
    const float *end,
    int entNum,
    int contentMask,
    unsigned __int8 *priorityMap,
    float *forward);
int Player_CheckAlmostStationary(gentity_s *ent, float *dir);
void Player_DebugDrawLOS(const float *center, const float *dir, double dist2D, int debugDrawDuration);
void Player_BanNodesInFront(gentity_s *ent, float dist, const float *start, const float *dir);
void Player_BlockFriendliesInADS(gentity_s *ent, float dist, const float *start, const float *dir);
void Player_GrenadeThrowBlockFriendlies(
    gentity_s *ent,
    float dist,
    const float *start,
    const float *dir);
void Player_UpdateLookAtEntity(gentity_s *ent);
