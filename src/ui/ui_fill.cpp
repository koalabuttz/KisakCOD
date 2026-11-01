#include "ui_fill.h"
#include <client/client.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#elif defined(KISAK_SP)
#include "ui.h"
#endif
void UI_FillRectMaterial(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float width,
    float height,
    int horzAlign,
    int vertAlign,
    const float *color,Material *whiteMaterial)
{
    if (whiteMaterial)
        CL_DrawStretchPic(
            scrPlace,
            x,
            y,
            width,
            height,
            horzAlign,
            vertAlign,
            0.0,
            0.0,
            0.0,
            0.0,
            color,
            whiteMaterial);
}
