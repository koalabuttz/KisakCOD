#pragma once
#include <bgame/bg_local.h>

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

enum CompassType : __int32;

struct alignas(4) CompassActor
{
    int lastUpdate;
    float lastPos[2];
    float lastYaw;
    int beginFadeTime;
    bool enemy;
};

struct CompassVehicle
{
    int entityNum;
    int lastUpdate;
    float lastPos[2];
    float lastYaw;
};

void TRACK_cg_compassfriendlies();
void CG_ClearCompassPingData();
void CG_CompassAddWeaponPingInfo(int localClientNum, centity_s *cent, float *origin, int msec);
void CG_CompassApplyPointerRadiusScale(float *radiusScale);
void CG_CalcCompassPointerRadius(float *radius, double dist);
void CG_CompassUpdateActorInfo(int localClientNum, int entityIndex);
CompassVehicle *Compass_GetVehicle(int localClientNum, int entityNum);
void CG_CompassUpdateVehicleInfo(int localClientNum, int entityIndex);
void CG_CompassDrawActors(
    int localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    float *color);
void CG_CompassDrawVehicles(
    int localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    float *color,
    unsigned __int8 vehicleCompassType);
