#include <universal/q_shared.h>
#include "ui_shared.h"
#include <client/client.h>

void __cdecl UI_DrawWrappedText(
    const ScreenPlacement *scrPlace,
    const char *text,
    const rectDef_s *rect,
    Font_s *font,
    float x,
    float y,
    float scale,
    const float *color,
    int style,
    char textAlignMode,
    rectDef_s *textRect)
{
    DrawWrappedText(scrPlace, text, rect, font, x, y, scale, color, style, textAlignMode, textRect, 0, 0, 0);
}

void __cdecl DrawWrappedText(
    const ScreenPlacement *scrPlace,
    const char *text,
    const rectDef_s *rect,
    Font_s *font,
    float x,
    float y,
    float scale,
    const float *color,
    int style,
    char textAlignMode,
    rectDef_s *textRect,
    bool subtitle,
    const float *subtitleGlowColor,
    bool cinematic)
{
    float v14; // [esp+2Ch] [ebp-44Ch]
    float v15; // [esp+30h] [ebp-448h]
    float v16; // [esp+34h] [ebp-444h]
    float v17; // [esp+40h] [ebp-438h]
    float lineX; // [esp+44h] [ebp-434h]
    float height; // [esp+4Ch] [ebp-42Ch]
    float normalizedScale; // [esp+50h] [ebp-428h]
    float textWidth; // [esp+54h] [ebp-424h]
    int len; // [esp+58h] [ebp-420h]
    int targetLineWidth; // [esp+5Ch] [ebp-41Ch]
    const char *wrapPosition; // [esp+60h] [ebp-418h]
    float lineWidth; // [esp+64h] [ebp-414h]
    char buff[1028]; // [esp+68h] [ebp-410h] BYREF
    int xAlignMode; // [esp+470h] [ebp-8h]
    const char *p; // [esp+474h] [ebp-4h]
    float xa; // [esp+490h] [ebp+18h]
    float ya; // [esp+494h] [ebp+1Ch]

    if (rect == textRect)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 4656, 0, "%s", "rect != textRect");
    textRect->x = x + rect->w;
    textRect->y = y;
    textRect->w = 0.0;
    textRect->h = 0.0;
    normalizedScale = R_NormalizedTextScale(font, scale);
    height = (float)UI_TextHeight(font, scale);
    textWidth = (float)UI_TextWidth(text, 0, font, scale);
    xAlignMode = textAlignMode & 3;
    xa = Item_GetTextPlacementX(xAlignMode, x, rect->w, 0.0);
    ya = Item_GetTextPlacementY(textAlignMode & 0xC, y, rect->h, height);
    if (rect->w == 0.0 || rect->w >= (double)textWidth || xAlignMode != 1)
    {
        targetLineWidth = (int)rect->w;
    }
    else
    {
        v17 = textWidth / rect->w;
        v16 = ceil(v17);
        v15 = textWidth / (double)(int)v16;
        v14 = ceil(v15);
        targetLineWidth = UI_PickWordWrapLineWidth(text, 1024, font, normalizedScale, (int)v16, (int)v14, (int)rect->w);
    }
    p = text;
    while (*p)
    {
        wrapPosition = R_TextLineWrapPosition(p, 1024, targetLineWidth, font, normalizedScale);
        len = wrapPosition - p;
        memcpy((uint8_t *)buff, (uint8_t *)p, wrapPosition - p);
        buff[len] = 0;
        lineWidth = (float)UI_TextWidth(buff, 0, font, scale);
        if (xAlignMode == 1)
        {
            lineX = xa - lineWidth * 0.5;
        }
        else if (xAlignMode == 2)
        {
            lineX = xa - lineWidth;
        }
        else
        {
            lineX = xa;
        }
        if (subtitle)
        {
            if (!subtitleGlowColor)
                MyAssertHandler(".\\ui\\ui_shared.cpp", 4702, 0, "%s", "subtitleGlowColor");
            UI_DrawTextWithGlow(
                scrPlace,
                buff,
                0x7FFFFFFF,
                font,
                lineX,
                ya,
                rect->horzAlign,
                rect->vertAlign,
                scale,
                color,
                style,
                subtitleGlowColor,
                1,
                cinematic);
        }
        else
        {
            UI_DrawText(scrPlace, buff, 0x7FFFFFFF, font, lineX, ya, rect->horzAlign, rect->vertAlign, scale, color, style);
        }
        ya = height + 5.0 + ya;
        if (lineWidth > (double)textRect->w)
        {
            if (lineX > (double)textRect->x)
                MyAssertHandler(".\\ui\\ui_shared.cpp", 4713, 0, "textRect->x >= lineX\n\t%g, %g", textRect->x, lineX);
            textRect->x = lineX;
            textRect->w = lineWidth;
        }
        for (p += len; isspace(*p); ++p)
            ;
    }
    textRect->h = ya - textRect->y;
}

double __cdecl Item_GetTextPlacementX(int alignX, float x0, float containerWidth, float selfWidth)
{
    if (!alignX)
        return x0;
    if (alignX == 1)
    {
        return (float)((containerWidth - selfWidth) * 0.5 + x0);
    }
    else
    {
        if (alignX != 2)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 191, 0, "alignX == ITEM_ALIGN_RIGHT\n\t%i, %i", alignX, 2);
        return (float)(containerWidth - selfWidth + x0);
    }
}

double __cdecl Item_GetTextPlacementY(int alignY, float y0, float containerHeight, float selfHeight)
{
    switch (alignY)
    {
    case 0:
        return y0;
    case 4:
        return (float)(y0 + selfHeight);
    case 8:
        return (float)((containerHeight + selfHeight) * 0.5 + y0);
    default:
        if (alignY != 12)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 215, 0, "%s\n\t(alignY) = %i", "(alignY == 12)", alignY);
        return (float)(y0 + containerHeight);
    }
}

int __cdecl UI_PickWordWrapLineWidth(
    const char *text,
    int bufferSize,
    Font_s *font,
    float normalizedScale,
    int targetLineCount,
    int widthGuess,
    int widthLimit)
{
    int lineCount; // [esp+4h] [ebp-8h]
    const char *p; // [esp+8h] [ebp-4h]

    while (widthGuess < widthLimit)
    {
        p = text;
        lineCount = 0;
        while (*p)
        {
            for (p = R_TextLineWrapPosition(p, bufferSize, widthGuess, font, normalizedScale); isspace(*p); ++p)
                ;
            ++lineCount;
        }
        if (lineCount == targetLineCount)
            return widthGuess;
        widthGuess += 8;
    }
    return widthLimit;
}

void __cdecl UI_DrawWrappedTextSubtitled(
    const ScreenPlacement *scrPlace,
    const char *text,
    const rectDef_s *rect,
    Font_s *font,
    float x,
    float y,
    float scale,
    const float *color,
    int style,
    char textAlignMode,
    rectDef_s *textRect,
    const float *subtitleGlowColor,
    bool cinematic)
{
    DrawWrappedText(
        scrPlace,
        text,
        rect,
        font,
        x,
        y,
        scale,
        color,
        style,
        textAlignMode,
        textRect,
        1,
        subtitleGlowColor,
        cinematic);
}

void __cdecl ToWindowCoords(float *x, float *y, const windowDef_t *window)
{
    if (window->border)
    {
        *x = *x + window->borderSize;
        *y = *y + window->borderSize;
    }
    if (!window)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    *x = *x + window->rect.x;
    *y = *y + window->rect.y;
}

void __cdecl Item_Text_AutoWrapped_Paint(
    int localClientNum,
    itemDef_s *item,
    const char *text,
    const float *color,
    bool subtitle,
    const float *subtitleGlowColor,
    bool cinematic)
{
    Font_s *font; // [esp+24h] [ebp-10h]
    float x; // [esp+28h] [ebp-Ch] BYREF
    float y; // [esp+2Ch] [ebp-8h] BYREF
    const rectDef_s *rect; // [esp+30h] [ebp-4h]

    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    rect = &item->window.rect;
    font = UI_GetFontHandle(&scrPlaceView[localClientNum], item->fontEnum, item->textscale);
    x = item->textalignx;
    y = item->textaligny;
    ToWindowCoords(&x, &y, &item->window);
    if (subtitle)
    {
        if (!subtitleGlowColor)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 4756, 0, "%s", "subtitleGlowColor");
        UI_DrawWrappedTextSubtitled(
            &scrPlaceView[localClientNum],
            text,
            rect,
            font,
            x,
            y,
            item->textscale,
            color,
            item->textStyle,
            item->textAlignMode,
            &item->textRect[localClientNum],
            subtitleGlowColor,
            cinematic);
    }
    else
    {
        UI_DrawWrappedText(
            &scrPlaceView[localClientNum],
            text,
            rect,
            font,
            x,
            y,
            item->textscale,
            color,
            item->textStyle,
            item->textAlignMode,
            &item->textRect[localClientNum]);
    }
}
