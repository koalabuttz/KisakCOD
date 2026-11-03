#pragma once
#include <bgame/bg_local.h>

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

int CG_GetEntityIndex(const centity_s *cent);
int CG_GetLocalClientTime(int localClientNum);
void CG_Actor_PreControllers(centity_s *cent);
void CG_Actor(int localClientNum, centity_s *cent);
void CG_ActorSpawner(centity_s *cent);
