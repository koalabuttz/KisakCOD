#include <universal/q_shared.h>
#include "r_font.h"

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
