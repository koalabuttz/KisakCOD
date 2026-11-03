#pragma once
#include "r_rendercmds.h"

void TRACK_rb_sunshadow();
void RB_SunShadowMaps(const GfxBackEndData *data, const GfxViewInfo *viewInfo);
void RB_DrawSunShadowOverlay();
void RB_GetShadowOverlayDepthBounds(float *nearDepth, float *farDepth); // used in rb_spotshadow??