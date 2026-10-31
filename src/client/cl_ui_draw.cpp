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
