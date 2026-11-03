#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

void TRACK_cl_srcn();
void SCR_DrawSmallStringExt(unsigned int x, int y, const char *string, const float *setColor);
void SCR_Init();
float CL_GetMenuBlurRadius(int localClientNum);
void SCR_UpdateScreen();
void SCR_UpdateFrame();
int CL_CGameRendering();


bool CL_IsCGameRendering();
void CL_DrawScreen();
void SCR_DrawScreenField(int refreshedUI);
void SCR_UpdateRumble();
void SCR_UpdateLoadScreen();
void CL_CubemapShotUsage();
void CL_CubemapShot_f();
