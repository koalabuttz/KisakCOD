#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct XAnimTree_s;
struct gentity_s;

XAnimTree_s *G_GetActorCorpseIndexAnimTree(unsigned int index);
int G_GetActorCorpseIndex(gentity_s *ent);
int G_GetFreeActorCorpseIndex(int reuse);
void G_RemoveActorCorpses(unsigned int allowedCorpseCount);
void G_UpdateActorCorpses();
int G_PruneCorpsesSortCmp(int *a, int *b);
void G_PruneLoadedCorpses();
void ActorCorpse_Free(gentity_s *ent);
void Actor_CorpseThink(gentity_s *self);
float Actor_SetBodyPlantAngle(
    int iEntNum,
    int iClipMask,
    float *vOrigin,
    const float *vCenter,
    const float *vDir,
    float *pfAngle);
void Actor_GetBodyPlantAngles(
    int iEntNum,
    int iClipMask,
    float *vOrigin,
    float fYaw,
    float *pfPitch,
    float *pfRoll,
    float *pfHeight);
float Actor_Orient_LerpWithLimit(float current, float newValue, float delta, float rate);
void Actor_OrientCorpseToGround(gentity_s *self, int bLerp);
void Actor_OrientPitchToGround(gentity_s *self, int bLerp);
int Actor_BecomeCorpse(gentity_s *self);
XAnimTree_s *G_GetActorCorpseAnimTree(gentity_s *ent);

extern float playerEyePos[3];