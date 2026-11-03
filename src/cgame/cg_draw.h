#pragma once
#include "cg_local.h"


#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

enum BlurTime : __int32
{
    BLUR_TIME_RELATIVE = 0x0,
    BLUR_TIME_ABSOLUTE = 0x1,
};

enum BlurPriority : __int32
{
    BLUR_PRIORITY_NONE = 0x0,
    BLUR_PRIORITY_SCRIPT = 0x1,
    BLUR_PRIORITY_CODE = 0x2,
};

struct CenterPrint
{
    int time;
    char text[1024];
};

struct ScreenBlur
{
    BlurPriority priority;
    BlurTime time;
    int timeStart;
    int timeEnd;
    float start;
    float end;
    float radius;
};

struct ScreenFade
{
    float alpha;
    float alphaCurrent;
    int startTime;
    int duration;
};

weaponInfo_s *CG_GetLocalClientWeaponInfo(int localClientNum, int weaponIndex);
void TRACK_cg_draw();
void CG_CenterPrint(int localClientNum, const char *str);
void CG_DrawCenterString(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    double fontscale,
    float *color,
    int textStyle);
int CG_DrawFriendlyFire(const cg_s *cgameGlob);
void CG_DrawFlashFade(int localClientNum);
int CG_CheckPlayerMovement(
    __int64 newCmd,
    __int64 a2,
    __int64 a3,
    __int64 a4,
    __int64 a5,
    __int64 a6,
    __int64 a7,
    __int64 a8,
    __int64 a9,
    __int64 a10,
    __int64 a11,
    __int64 a12,
    __int64 a13,
    __int64 a14,
    int a15,
    int a16,
    int a17,
    __int16 a18);
int CG_CheckPlayerStanceChange(int localClientNum, __int16 newButtons, __int16 changedButtons);
int CG_CheckPlayerTryReload(int localClientNum, char buttons);
int CG_CheckPlayerFireNonTurret(int localClientNum, char buttons);
int CG_CheckPlayerWeaponUsage(int localClientNum, char buttons);
int CG_CheckPlayerOffHandUsage(int localClientNum, __int16 buttons);
unsigned int CG_CheckPlayerMiscInput(int buttons);
void CG_CheckForPlayerInput(int localClientNum);
void CG_CheckHudHealthDisplay(int localClientNum);
void CG_CheckHudAmmoDisplay(int localClientNum);
void CG_CheckHudCompassDisplay(int localClientNum);
void CG_CheckHudStanceDisplay(int localClientNum);
void CG_CheckHudSprintDisplay(int localClientNum);
void CG_CheckHudOffHandDisplay(int localClientNum);
void CG_CheckHudObjectiveDisplay(int localClientNum);
void CG_CheckTimedMenus(int localClientNum);
void CG_Blur(
    int localClientNum,
    int time,
    double endBlur,
    BlurTime timeType,
    BlurTime priority,
    BlurPriority a6);
void CG_ClearBlur(int localClientNum);
float CG_GetBlurRadius(int localClientNum);
void CG_ScreenBlur(int localClientNum);
void CG_Fade(int localClientNum, int r, int g, int b, int a, int startTime, int duration);
void CG_DrawMiniConsole();
void CG_DrawErrorMessages();
void CG_DrawFadeInCinematic(int localClientNum);
void CG_DrawFriendOverlay(int localClientNum);
void CG_DrawPaused(int localClientNum);
void CG_AlterTimescale(int localClientNum, int time, double startScale, double endScale);
void CG_UpdateTimeScale(int localClientNum);
void DrawFontTest(int localClientNum);
void DrawViewmodelInfo(int localClientNum);
void CG_Draw2D(int localClientNum);
void CG_DrawActive(int localClientNum);
void CG_AddSceneTracerBeams(int localClientNum);
void CG_GenerateSceneVerts(int localClientNum);
