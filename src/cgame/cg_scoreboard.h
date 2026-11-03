#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "cg_local.h"

float CG_FadeObjectives(const struct cg_s *cgameGlob);
void CG_DrawObjectiveBackdrop(const struct cg_s *cgameGlob, const float *color);
void CG_DrawObjectiveHeader(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    double scale,
    float *color,
    int textStyle);
const char *CG_WordWrap(
    const char *inputText,
    Font_s *font,
    double scale,
    int maxChars,
    int maxPixelWidth,
    int *charCount,
    int *a7);
void CG_DrawObjectiveList(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    double scale,
    float *color,
    int textStyle);
void CG_DrawPausedMenuLine(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    double scale,
    float *color,
    int textStyle);
int CG_DrawScoreboard(int localClientNum);
void CG_ParseObjectiveChange(int localClientNum, unsigned int num);
