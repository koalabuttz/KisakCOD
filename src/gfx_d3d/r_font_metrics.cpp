#include <universal/q_shared.h>
#include "r_font.h"
#include <stringed/stringed_hooks.h>

const Glyph *__cdecl R_GetCharacterGlyph(Font_s *font, uint32_t letter)
{
    Glyph *glyph; // [esp+0h] [ebp-10h]
    int top; // [esp+4h] [ebp-Ch]
    int bottom; // [esp+8h] [ebp-8h]
    int mid; // [esp+Ch] [ebp-4h]

    if (letter < 0x20 || letter > 0x7F)
    {
        top = font->glyphCount - 1;
        bottom = 96;
        while (bottom <= top)
        {
            mid = (bottom + top) / 2;
            if (font->glyphs[mid].letter == letter)
                return &font->glyphs[mid];
            if (font->glyphs[mid].letter >= letter)
                top = mid - 1;
            else
                bottom = mid + 1;
        }
        return font->glyphs + 14;
    }
    else
    {
        glyph = &font->glyphs[letter - 32];
        iassert( glyph->letter == letter );
        return glyph;
    }
}

double __cdecl R_NormalizedTextScale(Font_s *font, float scale)
{
    iassert( font );
    iassert( font->pixelHeight > 0 );
    return (float)(scale * 48.0 / (double)font->pixelHeight);
}

int __cdecl R_LetterWidth(uint32_t letter, Font_s *font)
{
    return R_GetCharacterGlyph(font, letter)->dx;
}

int __cdecl R_TextWidth(const char *text, int maxChars, Font_s *font)
{
    uint32_t letter; // [esp+0h] [ebp-10h]
    int lineWidth; // [esp+4h] [ebp-Ch]
    int maxWidth; // [esp+8h] [ebp-8h]
    int count; // [esp+Ch] [ebp-4h]

    iassert( text );
    iassert( font );
    lineWidth = 0;
    maxWidth = 0;
    if (maxChars <= 0)
        maxChars = 0x7FFFFFFF;
    count = 0;
    while (*text && count < maxChars)
    {
        letter = SEH_ReadCharFromString(&text, 0);
        if (letter == 13 || letter == 10)
        {
            lineWidth = 0;
        }
        else if (letter == 94 && text && *text != 94 && *text >= 48 && *text <= 57)
        {
            ++text;
        }
        else
        {
            lineWidth += R_GetCharacterGlyph(font, letter)->dx;
            if (maxWidth < lineWidth)
                maxWidth = lineWidth;
            ++count;
        }
    }
    return maxWidth;
}

int __cdecl R_TextHeight(Font_s *font)
{
    iassert( font );
    return font->pixelHeight;
}

const char *__cdecl R_TextLineWrapPosition(
    const char *text,
    int bufferSize,
    int pixelsAvailable,
    Font_s *font,
    float scale)
{
    int pixelsUsed; // [esp+Ch] [ebp-14h]
    const char *preLetterPos; // [esp+10h] [ebp-10h]
    const char *wrapPos; // [esp+14h] [ebp-Ch]
    const char *parsePos; // [esp+18h] [ebp-8h] BYREF
    uint32_t letter; // [esp+1Ch] [ebp-4h]

    iassert( text );
    pixelsUsed = 0;
    if (bufferSize <= 0)
        bufferSize = 0x7FFFFFFF;
    wrapPos = 0;
    preLetterPos = 0;
    parsePos = text;
    while (*parsePos)
    {
        preLetterPos = parsePos;
        letter = SEH_ReadCharFromString(&parsePos, 0);
        if (letter == 13)
        {
            pixelsUsed = 0;
        }
        else
        {
            if (letter == 10)
                return parsePos;
            if (letter == 94 && parsePos && *parsePos != 94 && *parsePos >= 48 && *parsePos <= 57)
            {
                ++parsePos;
            }
            else if (letter == 94 && (*parsePos == 1 || *parsePos == 2))
            {
                if (font)
                    pixelsUsed += (font->pixelHeight * (parsePos[1] - 16) + 16) / 32;
                if (preLetterPos != text)
                    wrapPos = preLetterPos;
                parsePos += 7;
            }
            else
            {
                if (font)
                    pixelsUsed += R_LetterWidth(letter, font);
                if (preLetterPos != text && letter < 0x100 && isspace(letter))
                    wrapPos = preLetterPos;
            }
            if (wrapPos && (double)pixelsAvailable < (double)pixelsUsed * scale)
                return wrapPos;
            if (parsePos - text > bufferSize)
            {
                if (wrapPos)
                    return wrapPos;
                else
                    return preLetterPos;
            }
        }
    }
    if (parsePos - text != bufferSize)
        return parsePos;
    if (wrapPos)
        return wrapPos;
    else
        return preLetterPos;
}
