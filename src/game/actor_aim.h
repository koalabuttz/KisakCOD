#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <qcommon/graph.h>

enum WeapAccuracyType : __int32;

enum enumLastShot : __int32
{
    LAST_SHOT_IN_CLIP = 0x0,
    NOT_LAST_SHOT_IN_CLIP = 0x1,
};

struct WeaponDef;
struct actor_s;
struct sentient_s;
struct weaponParms;
struct DevGraph;


void TRACK_actor_aim();
void Actor_DrawDebugAccuracy(const float *pos, double scale, double rowHeight);
void Actor_DebugAccuracyMsg(
    unsigned int msgIndex,
    const char *msg,
    float accuracy,
    const float *color);
float Actor_GetAccuracyFraction(
    float dist,
    const WeaponDef *weapDef,
    WeapAccuracyType accuracyType);
float Actor_GetWeaponAccuracy(
    const actor_s *self,
    const sentient_s *enemy,
    const WeaponDef *weapDef,
    WeapAccuracyType accuracyType);
float Actor_GetPlayerStanceAccuracy(const actor_s *self, const sentient_s *enemy);
float Actor_GetPlayerMovementAccuracy(const actor_s *self, const sentient_s *enemy);
float Actor_GetPlayerSightAccuracy(actor_s *self, const sentient_s *enemy);
float Actor_GetFinalAccuracy(actor_s *self, weaponParms *wp, double accuracyMod);
void Actor_FillWeaponParms(actor_s *self, weaponParms *wp);
void Actor_HitSentient(weaponParms *wp, sentient_s *enemy, float accuracy);
void Actor_HitTarget(const weaponParms *wp, const float *target, float *forward);
void Actor_HitEnemy(actor_s *self, weaponParms *wp, double accuracy);
void Actor_MissSentient(weaponParms *wp, sentient_s *enemy, float accuracy);
void Actor_MissTarget(const weaponParms *wp, const float *target, float *forward);
void Actor_MissEnemy(actor_s *self, weaponParms *wp, double accuracy);
void Actor_ShootNoEnemy(actor_s *self, weaponParms *wp);
void Actor_ShootPos(actor_s *self, weaponParms *wp, float *pos);
void Actor_ClampShot(actor_s *self, weaponParms *wp);
void Actor_Shoot(actor_s *self, float accuracyMod, float (*posOverride)[3], enumLastShot lastShot);
void Actor_ShootBlank(actor_s *self);
struct gentity_s *Actor_Melee(actor_s *self, const float *direction);
float Sentient_GetScarinessForDistance(sentient_s *self, sentient_s *enemy, double fDist);
void Actor_GetAccuracyGraphFileName_FastFile(
    const WeaponDef *weaponDef,
    WeapAccuracyType accuracyType,
    char *filePath,
    unsigned int sizeofFilePath);
void Actor_GetAccuracyGraphFileName(
    const WeaponDef *weaponDef,
    WeapAccuracyType accuracyType,
    char *filePath,
    unsigned int sizeofFilePath);
void Actor_AccuracyGraphSaveToFile(
    const DevGraph *graph,
    WeaponDef *weaponDef,
    WeapAccuracyType accuracyType);
void Actor_CommonAccuracyGraphEventCallback(
    const DevGraph *graph,
    DevEventType event,
    WeapAccuracyType accuracyType);
void Actor_AiVsAiAccuracyGraphEventCallback(
    const DevGraph *graph,
    DevEventType event,
    int unusedLocalClientNum);
void Actor_AiVsPlayerAccuracyGraphEventCallback(
    const DevGraph *graph,
    DevEventType event,
    int unusedLocalClientNum);
void Actor_AccuracyGraphTextCallback(
    const DevGraph *graph,
    const float inputX,
    const float inputY,
    char *text,
    const int textLength);
void G_SwapAccuracyBuffers();
DevGraph *Actor_InitWeaponAccuracyGraphForWeaponType(
    unsigned int weaponIndex,
    WeapAccuracyType accuracyType,
    void(*eventCallback)(const DevGraph *, DevEventType, int));
void Actor_CopyAccuGraphBuf(WeaponDef *from, WeaponDef *to);
void Actor_InitWeaponAccuracyGraphForWeapon(unsigned int weaponIndex);
void Actor_ShutdownWeaponAccuracyGraph();
