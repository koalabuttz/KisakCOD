#include <universal/q_shared.h>
#include "ui_shared.h"
#include <client/client.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#endif
#include <universal/profile.h>

void __cdecl UI_DrawText(
    const ScreenPlacement *scrPlace,
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    int horzAlign,
    int vertAlign,
    float scale,
    const float *color,
    int style)
{
    PROF_SCOPED("UI_DrawText");

    float v11; // [esp+18h] [ebp-18h]
    float v12; // [esp+1Ch] [ebp-14h]
    float v13; // [esp+20h] [ebp-10h]
    float v14; // [esp+24h] [ebp-Ch]
    float xScale; // [esp+28h] [ebp-8h] BYREF
    float yScale; // [esp+2Ch] [ebp-4h] BYREF

    xScale = R_NormalizedTextScale(font, scale);
    yScale = xScale;
    ScrPlace_ApplyRect(scrPlace, &x, &y, &xScale, &yScale, horzAlign, vertAlign);
    v14 = x + 0.5;
    v12 = floor(v14);
    x = v12;
    v13 = y + 0.5;
    v11 = floor(v13);
    y = v11;
    CL_DrawTextPhysical(text, maxChars, font, x, v11, xScale, yScale, color, style);
}
