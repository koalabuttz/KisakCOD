#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "cg_local.h"

void TRACK_cg_predict();
void CG_InterpolatePlayerState(int localClientNum, int grabAngles, int grabStance);
void CG_RestorePlayerOrientation(cg_s *cgameGlob);
void CG_UpdateFreeMove(cg_s *cgameGlob);
void CG_InterpolateGroundTilt(int localClientNum);
void CG_PredictPlayerState_Internal(int localClientNum);
void CG_PredictPlayerState(int localClientNum);
