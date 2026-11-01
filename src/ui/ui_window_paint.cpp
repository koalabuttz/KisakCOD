#include <universal/q_shared.h>
#include "ui_shared.h"
#include "ui_input_core.h"
#include "ui_window_services.h"
#include <client/client.h>
#include <universal/profile.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#elif defined(KISAK_SP)
#include "ui.h"
#endif

void __cdecl Window_Paint(
    UiContext *dc,
    windowDef_t *w,
    float fadeAmount,
    float fadeInAmount,
    float fadeClamp,
    float fadeCycle)
{
    PROF_SCOPED("Window_Paint");

    float *v6; // [esp+24h] [ebp-80h]
    float *v7; // [esp+28h] [ebp-7Ch]
    float *v8; // [esp+2Ch] [ebp-78h]
    int v9; // [esp+38h] [ebp-6Ch]
    int v10; // [esp+40h] [ebp-64h]
    int v11; // [esp+48h] [ebp-5Ch]
    int localClientNum; // [esp+50h] [ebp-54h]
    float fillRect; // [esp+6Ch] [ebp-38h]
    float fillRect_4; // [esp+70h] [ebp-34h]
    float fillRect_8; // [esp+74h] [ebp-30h]
    float fillRect_12; // [esp+78h] [ebp-2Ch]
    float lowColor[4]; // [esp+84h] [ebp-20h] BYREF
    const ScreenPlacement *scrPlace; // [esp+94h] [ebp-10h]
    const float *foreColor; // [esp+98h] [ebp-Ch]
    int flags; // [esp+9Ch] [ebp-8h] BYREF
    const rectDef_s *origRect; // [esp+A0h] [ebp-4h]

    if (!w)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    origRect = &w->rect;
    fillRect = w->rect.x;
    fillRect_4 = w->rect.y;
    fillRect_8 = w->rect.w;
    fillRect_12 = w->rect.h;
    scrPlace = &scrPlaceView[dc->localClientNum];
    if (g_debugMode)
        UI_WindowDrawRect(
            scrPlace,
            origRect->x,
            origRect->y,
            origRect->w,
            origRect->h,
            origRect->horzAlign,
            origRect->vertAlign,
            1.0,
            colorWhite);
    if (w && (w->style || w->border))
    {
        if (w->border)
        {
            fillRect = fillRect + w->borderSize;
            fillRect_4 = fillRect_4 + w->borderSize;
            fillRect_8 = fillRect_8 - (w->borderSize + 1.0);
            fillRect_12 = fillRect_12 - (w->borderSize + 1.0);
        }
        switch (w->style)
        {
        case 1:
            if (w->background)
            {
                localClientNum = dc->localClientNum;
                if (dc->localClientNum)
                    MyAssertHandler(
                        "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                        23,
                        0,
                        "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                        localClientNum,
                        1);
                flags = w->dynamicFlags[localClientNum];
                Fade(&flags, &w->backColor[3], fadeClamp, &w->nextTime, (int)fadeCycle, 1, fadeAmount, fadeInAmount, dc);
                Window_SetDynamicFlags(dc->localClientNum, w, flags);
                UI_DrawHandlePic(
                    scrPlace,
                    fillRect,
                    fillRect_4,
                    fillRect_8,
                    fillRect_12,
                    origRect->horzAlign,
                    origRect->vertAlign,
                    w->backColor,
                    w->background);
            }
            else
            {
                UI_WindowFillRect(
                    scrPlace,
                    fillRect,
                    fillRect_4,
                    fillRect_8,
                    fillRect_12,
                    origRect->horzAlign,
                    origRect->vertAlign,
                    w->backColor);
            }
            break;
        case 2:
            KISAK_NULLSUB();
            break;
        case 3:
            v11 = dc->localClientNum;
            if (dc->localClientNum)
                MyAssertHandler(
                    "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                    23,
                    0,
                    "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                    v11,
                    1);
            if ((w->dynamicFlags[v11] & 0x10000) != 0)
                v8 = w->foreColor;
            else
                v8 = 0;
            foreColor = v8;
            UI_DrawHandlePic(
                scrPlace,
                fillRect,
                fillRect_4,
                fillRect_8,
                fillRect_12,
                origRect->horzAlign,
                origRect->vertAlign,
                v8,
                w->background);
            break;
        case 5:
            if (w->background)
            {
                v9 = dc->localClientNum;
                if (dc->localClientNum)
                    MyAssertHandler(
                        "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                        23,
                        0,
                        "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                        v9,
                        1);
                if ((w->dynamicFlags[v9] & 0x10000) != 0)
                    v6 = w->foreColor;
                else
                    v6 = 0;
                foreColor = v6;
                UI_DrawHandlePic(
                    scrPlace,
                    fillRect,
                    fillRect_4,
                    fillRect_8,
                    fillRect_12,
                    origRect->horzAlign,
                    origRect->vertAlign,
                    v6,
                    w->background);
            }
            break;
        case 6:
            v10 = dc->localClientNum;
            if (dc->localClientNum)
                MyAssertHandler(
                    "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                    23,
                    0,
                    "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                    v10,
                    1);
            if ((w->dynamicFlags[v10] & 0x10000) != 0)
                v7 = w->foreColor;
            else
                v7 = 0;
            foreColor = v7;
            UI_WindowDrawLoadBar(
                scrPlace,
                fillRect,
                fillRect_4,
                fillRect_8,
                fillRect_12,
                origRect->horzAlign,
                origRect->vertAlign,
                v7,
                w->background);
            break;
        default:
            break;
        }
        switch (w->border)
        {
        case 1:
            UI_WindowDrawRect(
                scrPlace,
                origRect->x,
                origRect->y,
                origRect->w,
                origRect->h,
                origRect->horzAlign,
                origRect->vertAlign,
                w->borderSize,
                w->borderColor);
            break;
        case 2:
            UI_WindowDrawTopBottom(
                scrPlace,
                origRect->x,
                origRect->y,
                origRect->w,
                origRect->h,
                origRect->horzAlign,
                origRect->vertAlign,
                w->borderSize,
                w->borderColor);
            break;
        case 3:
            UI_WindowDrawSides(
                scrPlace,
                origRect->x,
                origRect->y,
                origRect->w,
                origRect->h,
                origRect->horzAlign,
                origRect->vertAlign,
                w->borderSize,
                w->borderColor);
            break;
        case 4:
            KISAK_NULLSUB();
            KISAK_NULLSUB();
            break;
        case 5:
            Vec3Scale(w->borderColor, UI_WindowBorderLowLightScale(), lowColor);
            lowColor[3] = w->borderColor[3];
            UI_WindowDrawHighlightRect(
                scrPlace,
                origRect->x,
                origRect->y,
                origRect->w,
                origRect->h,
                origRect->horzAlign,
                origRect->vertAlign,
                w->borderSize,
                w->borderColor,
                lowColor);
            break;
        case 6:
            Vec3Scale(w->borderColor, UI_WindowBorderLowLightScale(), lowColor);
            lowColor[3] = w->borderColor[3];
            UI_WindowDrawHighlightRect(
                scrPlace,
                origRect->x,
                origRect->y,
                origRect->w,
                origRect->h,
                origRect->horzAlign,
                origRect->vertAlign,
                w->borderSize,
                lowColor,
                w->borderColor);
            break;
        }
    }
}
