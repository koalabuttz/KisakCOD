#include <universal/q_shared.h>
#include "client.h"
#include <gfx_d3d/r_rendercmds.h>

void __cdecl CL_DrawStretchPicPhysical(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material)
{
    R_AddCmdDrawStretchPic(x, y, w, h, s1, t1, s2, t2, color, material);
}

void __cdecl CL_DrawStretchPic(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int32_t horzAlign,
    int32_t vertAlign,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material)
{
    ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
    CL_DrawStretchPicPhysical(x, y, w, h, s1, t1, s2, t2, color, material);
}

void __cdecl CL_DrawTextPhysical(
    const char *text,
    int32_t maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int32_t style)
{
    R_AddCmdDrawText(text, maxChars, font, x, y, xScale, yScale, 0.0, color, style);
}

void __cdecl CL_SetViewport(int x, int y, int width, int height)
{
    R_AddCmdSetViewportValues(x, y, width, height);
}
