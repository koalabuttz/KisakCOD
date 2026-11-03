#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "cg_local.h"

struct TestEffect
{
    char name[64];
    float pos[3];
    int time;
    int respawnTime;
};

struct ClientViewParams
{
    float x;
    float y;
    float width;
    float height;
};

enum CubemapShot : __int32;

void TRACK_cg_view();
void CG_PlayTestFx(int localClientNum);
void CG_UpdateTestFX(int localClientNum);
void CG_FxSetTestPosition();
void CG_FxTest();
void CG_CalcVrect(int localClientNum);
// local variable allocation has failed, the output may be wrong!
void CG_SmoothCameraZ(cg_s *cgameGlob);
void CG_KickAngles(cg_s *cgameGlob);
float CG_GetVerticalBobFactor(
    const playerState_s *predictedPlayerState,
    float cycle,
    float speed,
    float maxAmp);
float CG_GetHorizontalBobFactor(
    const playerState_s *predictedPlayerState,
    float cycle,
    float speed,
    float maxAmp);
void CG_CalculateView_IdleAngles(cg_s *cgameGlob, float *angles);
void CG_CalculateView_BobAngles(const cg_s *cgameGlob, float *angles);
void CG_AddGroundTiltToAngles(int localClientNum, float *angles, const cg_s *cgameGlob);
void OffsetFirstPersonView(int localClientNum, cg_s *cgameGlob);
float CG_GetViewFov(int localClientNum);
void CG_CalcFov(int localClientNum);
float CG_GetViewZoomScale();
void CG_CalcCubemapViewValues(cg_s *cgameGlob);
void CG_CalcVehicleViewValues(int localClientNum);
void CalcTurretViewValues(int localClientNum);
void CG_CalcLinkedViewValues(int localClientNum);
void CG_ApplyViewAnimation(int localClientNum);
int PausedClientFreeMove(int localClientNum);
void CG_SetDebugOrigin(float *origin);
void CG_SetDebugAngles(const float *angles);
void CG_UpdateEntInfo(int localClientNum);
const ClientViewParams *CG_GetLocalClientViewParams(int localClientNum);
void CG_ArchiveViewInfo(cg_s *cgameGlob, MemoryFile *memFile);
void GetCeilingHeight(cg_s *cgameGlob);
void DumpAnims(int localClientNum);
void DrawShellshockBlend(int localClientNum);
void CG_UpdateViewOffset(int localClientNum);
void UpdateTurretScopeZoom(cg_s *cgameGlob);
void CG_UpdateSceneDepthOfField(cg_s *cgameGlob);
void CG_CalcViewValues(int localClientNum);
void CG_InitView(int localClientNum);
int CG_DrawActiveFrame(
    int localClientNum,
    int serverTime,
    DemoType demoType,
    CubemapShot cubemapShot,
    int cubemapSize,
    int animFrametime);
