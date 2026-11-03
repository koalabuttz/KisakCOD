#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "aim_assist.h"
#include <client/client.h>

void TRACK_aim_target();
const dvar_s *AimTarget_RegisterDvars(int a1, unsigned __int16 a2, const char *a3);
void AimTarget_Init();
void AimTarget_ClearTargetList();
int AimTarget_CompareTargets(const AimTarget *targetA, const AimTarget *targetB);
void AimTarget_AddTargetToList(const AimTarget *target);
void AimTarget_GetTargetBounds(const gentity_s *targetEnt, float *mins, float *maxs);
float AimTarget_GetTargetRadius(const gentity_s *targetEnt);
void AimTarget_GetTargetCenter(const gentity_s *targetEnt, float *center);
int AimTarget_IsTargetValid(const gentity_s *targetEnt);
int AimTarget_IsTargetVisible(const gentity_s *targetEnt, unsigned int visBone);
void AimTarget_CreateTarget(const gentity_s *targetEnt, AimTarget *target);
bool AimTarget_PlayerInValidState(const playerState_s *ps);
void AimTarget_ProcessEntity(gentity_s *ent);
void AimTarget_UpdateClientTargets();
void AimTarget_GetClientTargetList(AimTarget **targetList, int *targetCount);
int AimTarget_GetBestTarget(const float *start, const float *viewDir);
void AimTarget_WriteSaveGame(SaveGame *save);
void AimTarget_ReadSaveGame(SaveGame *save);
