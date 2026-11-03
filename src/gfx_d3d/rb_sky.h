#pragma once
#include "r_sky.h"


void TRACK_rb_sky();
uint32_t RB_CalcSunSpriteSamples();
void RB_DrawSun(uint32_t localClientNum);
void RB_DrawSunQuerySprite(SunFlareDynamic *sunFlare);
void RB_HW_BeginOcclusionQuery(IDirect3DQuery9 *query);
uint32_t RB_HW_ReadOcclusionQuery(IDirect3DQuery9 *query);
void RB_TessSunBillboard(float widthInClipSpace, float heightInClipSpace, GfxColor color);
GfxVertex *RB_SetTessQuad(GfxColor color);
void RB_UpdateSunVisibilityWithoutQuery(SunFlareDynamic *sunFlare);
double RB_GetSunSampleRectRelativeArea(int widthInPixels, int heightInPixels);
void RB_AddSunEffects(SunFlareDynamic *sunFlare);
void RB_DrawSunSprite();
void RB_FreeSunSpriteQueries();
void RB_DrawSunPostEffects(uint32_t localClientNum);
void RB_DrawSunFlare(SunFlareDynamic *sunFlare, int frameTime);
double R_UpdateOverTime(float fCurrent, float fGoal, int iFadeInTime, int iFadeOutTime, int frametime);
void RB_DrawSunFlareCore(float alpha, float sizeIn640x480);
void RB_DrawBlindAndGlare(SunFlareDynamic *sunFlare, int frameTime);
void RB_CalcSunBlind(SunFlareDynamic *sunFlare, int frameTime, float *blind, float *glare);
void RB_AllocSunSpriteQueries();
