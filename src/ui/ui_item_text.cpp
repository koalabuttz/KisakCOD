#include <universal/q_shared.h>
#include "ui_shared.h"
#include "ui_input_core.h"
#include <client/client.h>
#include <universal/profile.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#elif defined(KISAK_SP)
#include "ui.h"
#endif
extern const float MY_SUBTITLE_GLOWCOLOR[4] = { 0.0f, 0.3f, 0.0f, 1.0f };

void __cdecl Item_Text_Paint(UiContext *dc, itemDef_s *item)
{
    PROF_SCOPED("Item_Text_Paint");

    char *VariantString; // eax
    Font_s *font; // [esp+2Ch] [ebp-434h]
    bool subtitle; // [esp+33h] [ebp-42Dh]
    char *textPtr; // [esp+34h] [ebp-42Ch]
    float color[4]; // [esp+3Ch] [ebp-424h] BYREF
    const rectDef_s *rect; // [esp+4Ch] [ebp-414h]
    char text[1028]; // [esp+50h] [ebp-410h] BYREF
    const rectDef_s *textRect; // [esp+458h] [ebp-8h]
    bool cinematic; // [esp+45Fh] [ebp-1h]

    cinematic = (item->itemFlags & 2) != 0;
    subtitle = cinematic;
    if (cinematic)
	{
#ifdef KISAK_SP
		Item_Text_GetSubtitle(uiInfo.videoSubtitles, text);
		if (!text[0])
			return;
		textPtr = text;
#else
		return;
#endif
	}
    else if (item->text)
    {
        textPtr = (char *)item->text;
    }
    else if (item->textExp.numEntries)
    {
        textPtr = GetExpressionResultString(dc->localClientNum, &item->textExp);
    }
    else
    {
        if (!item->dvar)
            return;
        VariantString = (char *)Dvar_GetVariantString(item->dvar);
        I_strncpyz(text, VariantString, 1024);
        textPtr = text;
    }
    if (*textPtr == 64)
        textPtr = UI_SafeTranslateString(textPtr + 1);
    if (*textPtr)
    {
        Item_TextColor(dc, item, (float (*)[4])color);
        Item_SetTextExtents(dc->localClientNum, item, textPtr);
        if ((item->window.staticFlags & 0x800000) != 0)
        {
            Item_Text_AutoWrapped_Paint(dc->localClientNum, item, textPtr, color, subtitle, MY_SUBTITLE_GLOWCOLOR, cinematic);
        }
        else
        {
            textRect = Item_GetTextRect(dc->localClientNum, item);
            if (!item)
                MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
            rect = &item->window.rect;
            font = UI_GetFontHandle(&scrPlaceView[dc->localClientNum], item->fontEnum, item->textscale);
            if (subtitle)
                UI_DrawTextWithGlow(
                    &scrPlaceView[dc->localClientNum],
                    textPtr,
                    0x7FFFFFFF,
                    font,
                    textRect->x,
                    textRect->y,
                    rect->horzAlign,
                    rect->vertAlign,
                    item->textscale,
                    color,
                    item->textStyle,
                    MY_SUBTITLE_GLOWCOLOR,
                    1,
                    cinematic);
            else
                UI_DrawText(
                    &scrPlaceView[dc->localClientNum],
                    textPtr,
                    0x7FFFFFFF,
                    font,
                    textRect->x,
                    textRect->y,
                    rect->horzAlign,
                    rect->vertAlign,
                    item->textscale,
                    color,
                    item->textStyle);
        }
    }
}

void __cdecl Item_SetTextExtents(int localClientNum, itemDef_s *item, const char *text)
{
    bool v3; // [esp+20h] [ebp-40h]
    bool v4; // [esp+24h] [ebp-3Ch]
    Font_s *font; // [esp+28h] [ebp-38h]
    bool isOwnerDraw; // [esp+2Fh] [ebp-31h]
    float xAdj; // [esp+30h] [ebp-30h]
    rectDef_s newRect; // [esp+3Ch] [ebp-24h] BYREF
    const char *dvarString; // [esp+54h] [ebp-Ch]
    bool isDvarField; // [esp+5Bh] [ebp-5h]
    int xAlignMode; // [esp+5Ch] [ebp-4h]

    if (!text)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 4525, 0, "%s", "text");
    xAlignMode = item->textAlignMode & 3;
    isOwnerDraw = item->type == 8;
    v4 = item->type != 8 && item->dvar;
    isDvarField = v4;
    v3 = xAlignMode && (isOwnerDraw || isDvarField);
    if (v3 || Item_GetTextRect(localClientNum, item)->w == 0.0)
    {
        font = UI_GetFontHandle(&scrPlaceView[localClientNum], item->fontEnum, item->textscale);
        newRect.x = item->textalignx;
        newRect.y = item->textaligny;
        newRect.w = (float)UI_TextWidth(text, 0, font, item->textscale);
        newRect.h = (float)UI_TextHeight(font, item->textscale);
        newRect.horzAlign = item->window.rect.horzAlign;
        newRect.vertAlign = item->window.rect.vertAlign;
        if (xAlignMode)
        {
            if (xAlignMode != 2 && xAlignMode != 1)
                MyAssertHandler(
                    ".\\ui\\ui_shared.cpp",
                    4550,
                    0,
                    "%s\n\t(xAlignMode) = %i",
                    "(xAlignMode == 2 || xAlignMode == 1)",
                    xAlignMode);
            xAdj = item->window.rect.w - (double)UI_TextWidth(text, 0, font, item->textscale);
            if (isOwnerDraw)
            {
                xAdj = xAdj - (double)UI_OwnerDrawWidth(item->window.ownerDraw, font, item->textscale);
            }
            else if (isDvarField && Item_IsTextField(item))
            {
                dvarString = Dvar_GetVariantString(item->dvar);
                xAdj = xAdj - (double)UI_TextWidth(dvarString, 0, font, item->textscale);
            }
            if (xAlignMode == 1)
                xAdj = xAdj * 0.5;
            newRect.x = newRect.x + xAdj;
        }
        newRect.y = Item_GetTextPlacementY(item->textAlignMode & 0xC, newRect.y, item->window.rect.h, newRect.h);
        ToWindowCoords(&newRect.x, &newRect.y, &item->window);
        Item_SetTextRect(localClientNum, item, &newRect);
    }
}

void __cdecl Item_TextColor(UiContext *dc, itemDef_s *item, float (*newColor)[4])
{
    float *disableColor; // edx
    float v4; // [esp+1Ch] [ebp-4Ch]
    float v5; // [esp+20h] [ebp-48h]
    float t; // [esp+28h] [ebp-40h]
    float v7; // [esp+2Ch] [ebp-3Ch]
    float v8; // [esp+34h] [ebp-34h]
    float v9; // [esp+38h] [ebp-30h]
    int localClientNum; // [esp+4Ch] [ebp-1Ch]
    float lowLight[4]; // [esp+50h] [ebp-18h] BYREF
    menuDef_t *parent; // [esp+60h] [ebp-8h]
    int flags; // [esp+64h] [ebp-4h] BYREF

    parent = item->parent;
    localClientNum = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            23,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    flags = item->window.dynamicFlags[localClientNum];
    Fade(
        &flags,
        &item->window.foreColor[3],
        parent->fadeClamp,
        &item->window.nextTime,
        parent->fadeCycle,
        1,
        parent->fadeAmount,
        parent->fadeInAmount,
        dc);
    Window_SetDynamicFlags(dc->localClientNum, &item->window, flags);
    if (Window_HasFocus(dc->localClientNum, &item->window))
    {
        lowLight[0] = parent->focusColor[0] * 0.800000011920929;
        lowLight[1] = parent->focusColor[1] * 0.800000011920929;
        lowLight[2] = parent->focusColor[2] * 0.800000011920929;
        lowLight[3] = parent->focusColor[3] * 0.800000011920929;
        v9 = (float)(dc->realTime / 75);
        v7 = sin(v9);
        t = v7 * 0.5 + 0.5;
        LerpColor(parent->focusColor, lowLight, (float *)newColor, t);
    }
    else if (item->textStyle != 1 || ((dc->realTime / 256) & 1) != 0)
    {
        (*newColor)[0] = item->window.foreColor[0];
        (*newColor)[1] = item->window.foreColor[1];
        (*newColor)[2] = item->window.foreColor[2];
        (*newColor)[3] = item->window.foreColor[3];
    }
    else
    {
        lowLight[0] = item->window.foreColor[0] * 0.800000011920929;
        lowLight[1] = item->window.foreColor[1] * 0.800000011920929;
        lowLight[2] = item->window.foreColor[2] * 0.800000011920929;
        lowLight[3] = item->window.foreColor[3] * 0.800000011920929;
        v8 = (float)(dc->realTime / 75);
        v5 = sin(v8);
        v4 = v5 * 0.5 + 0.5;
        LerpColor(item->window.foreColor, lowLight, (float *)newColor, v4);
    }
    if (item->enableDvar
        && *item->enableDvar
        && item->dvarTest
        && *item->dvarTest
        && (item->dvarFlags & 3) != 0
        && !Item_EnableShowViaDvar(item, 1))
    {
        disableColor = parent->disableColor;
        (*newColor)[0] = parent->disableColor[0];
        (*newColor)[1] = disableColor[1];
        (*newColor)[2] = disableColor[2];
        (*newColor)[3] = disableColor[3];
    }
}

void __cdecl Item_YesNo_Paint(UiContext *dc, itemDef_s *item)
{
    const char *VariantString; // eax
    char *v3; // eax
    float v4; // [esp+0h] [ebp-8h]

    PROF_SCOPED("Item_YesNo_Paint");

    if (item->dvar)
    {
        VariantString = Dvar_GetVariantString(item->dvar);
        v4 = atof(VariantString);
    }
    else
    {
        v4 = 0.0;
    }
    if (v4 == 0.0)
        v3 = UI_SafeTranslateString("EXE_NO");
    else
        v3 = UI_SafeTranslateString("EXE_YES");
    item->text = v3;
    Item_Text_Paint(dc, item);
}

void __cdecl Fade(
    int *flags,
    float *f,
    float clamp,
    int *nextTime,
    int offsetTime,
    int bFlags,
    float fadeAmount,
    float fadeInAmount,
    UiContext *dc)
{
    if ((*flags & 0x30) != 0 && dc->realTime > *nextTime)
    {
        *nextTime = offsetTime + dc->realTime;
        if ((*flags & 0x10) != 0)
        {
            *f = *f - fadeAmount;
            if (bFlags)
            {
                if (*f <= 0.0)
                    *flags &= 0xFFFFFFEB;
            }
        }
        else
        {
            *f = *f + fadeInAmount;
            if (clamp <= (double)*f)
            {
                *f = clamp;
                if (bFlags)
                    *flags &= ~0x20u;
            }
        }
    }
}

void __cdecl LerpColor(float *a, float *b, float *c, float t)
{
    int i; // [esp+0h] [ebp-4h]

    for (i = 0; i < 4; ++i)
    {
        c[i] = (b[i] - a[i]) * t + a[i];
        if (c[i] >= 0.0)
        {
            if (c[i] > 1.0)
                c[i] = 1.0;
        }
        else
        {
            c[i] = 0.0;
        }
    }
}
