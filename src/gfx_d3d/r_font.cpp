#include <universal/q_shared.h>
#include "r_font.h"
#include <stringed/stringed_hooks.h>
#include <qcommon/mem_track.h>
#include <universal/com_files.h>
#include <universal/com_memory.h>
#include "r_init.h"
#include <database/database.h>

Font_s *registeredFont[16];
int registeredFontCount;
const char MYRANDOMCHARS[63] =
{
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890"
};



uint32_t __cdecl R_FontGetRandomLetter(Font_s *font, int seed)
{
    return MYRANDOMCHARS[RandWithSeed(&seed) % 0x3E];
}

void __cdecl TRACK_r_font()
{
    track_static_alloc_internal(registeredFont, 64, "registeredFont", 18);
}

Font_s *__cdecl R_RegisterFont(const char *name, int imageTrack)
{
    if (IsFastFileLoad())
        return R_RegisterFont_FastFile(name);
    else
        return R_RegisterFont_LoadObj(name, imageTrack);
}

Font_s *__cdecl R_RegisterFont_FastFile(const char *fontName)
{
    return DB_FindXAssetHeader(ASSET_TYPE_FONT, fontName).font;
}

Font_s *__cdecl R_LoadFont(const char *fontName, int imageTrack)
{
    Material *v3; // eax
    char glowMaterialName[64]; // [esp+0h] [ebp-68h] BYREF
    Font_s *font; // [esp+44h] [ebp-24h]
    int totalMemSize; // [esp+48h] [ebp-20h]
    int file; // [esp+4Ch] [ebp-1Ch] BYREF
    int materialNameOffset; // [esp+50h] [ebp-18h] BYREF
    const char *materialName; // [esp+54h] [ebp-14h]
    int fileLen; // [esp+58h] [ebp-10h]
    int varMemSize; // [esp+5Ch] [ebp-Ch]
    int fontNameOffset; // [esp+60h] [ebp-8h] BYREF
    char *variableFontData; // [esp+64h] [ebp-4h]

    fileLen = FS_FOpenFileRead(fontName, &file);
    if (fileLen >= 0)
    {
        if (fileLen >= 16)
        {
            varMemSize = fileLen - 16;
            totalMemSize = fileLen - 16 + 24;
            font = (Font_s *)Hunk_Alloc(totalMemSize, "R_LoadFont", 22);
            variableFontData = (char *)&font[1];
            FS_Read((uint8_t *)&fontNameOffset, 4u, file);
            FS_Read((uint8_t *)&font->pixelHeight, 4u, file);
            FS_Read((uint8_t *)&font->glyphCount, 4u, file);
            FS_Read((uint8_t *)&materialNameOffset, 4u, file);
            FS_Read((uint8_t *)variableFontData, varMemSize, file);
            FS_FCloseFile(file);
            font->glyphs = (Glyph *)variableFontData;
            font->fontName = &variableFontData[fontNameOffset - 16];
            materialName = &variableFontData[materialNameOffset - 16];
            font->material = Material_RegisterHandle((char *)materialName, imageTrack);
#ifdef KISAK_RADIANT
            // P5.5b: the editor never renders glow-pass text (it never sets
            // TEXT_RENDERFLAG_GLOW / 0x10 — the XY/Z/coord labels draw through the
            // plain RB_DrawText3D path), and the retail CoD4 install ships no
            // "<font>_glow" material for the editor font (raw/materials/fonts has
            // qerfont but not qerfont_glow — the glow variant was a modtools-only
            // asset). Requesting it just prints a benign "Couldn't find material
            // fonts/qerfont_glow" each launch. Alias the glow handle to the base
            // font material so it stays a VALID handle (no error, no null deref if
            // a glow pass is ever reached). SP/MP builds are unaffected.
            (void)glowMaterialName;
            v3 = font->material;
#else
            Com_sprintf(glowMaterialName, 0x3Fu, "%s_glow", materialName);
            v3 = Material_RegisterHandle(glowMaterialName, imageTrack);
#endif
            font->glowMaterial = v3;
            return font;
        }
        else
        {
            FS_FCloseFile(file);
            Com_PrintError(CON_CHANNEL_GFX, "ERROR: Font file '%s' too small\n", fontName);
            return 0;
        }
    }
    else
    {
        if (fileLen == -2)
            Com_PrintError(CON_CHANNEL_GFX, "ERROR: Couldn't find font in iwd files or localized directories '%s'\n", fontName);
        else
            Com_PrintError(CON_CHANNEL_GFX, "ERROR: Couldn't find font '%s'\n", fontName);
        return 0;
    }
}

Font_s *__cdecl R_RegisterFont_LoadObj(const char *fontName, int imageTrack)
{
    Font_s *font; // [esp+0h] [ebp-8h]
    int fontIndex; // [esp+4h] [ebp-4h]

    iassert( rg.registered );
    for (fontIndex = 0; fontIndex < registeredFontCount; ++fontIndex)
    {
        if (!I_stricmp(fontName, registeredFont[fontIndex]->fontName))
            return registeredFont[fontIndex];
    }
    if (registeredFontCount < 16)
    {
        font = R_LoadFont(fontName, imageTrack);
        if (!font)
            Com_Error(ERR_DROP, "R_RegisterFont: Error while reading font '%s'", fontName);
        registeredFont[registeredFontCount++] = font;
        return font;
    }
    else
    {
        Com_Error(ERR_DROP, "R_RegisterFont: Too many fonts registered already.\n");
        return 0;
    }
}



int __cdecl R_ConsoleTextWidth(const char *textPool, int poolSize, int firstChar, int charCount, Font_s *font)
{
    int indexMask; // [esp+0h] [ebp-18h]
    int parsePos; // [esp+4h] [ebp-14h]
    int width; // [esp+8h] [ebp-10h]
    uint32_t letter; // [esp+Ch] [ebp-Ch]
    int usedCharCount; // [esp+10h] [ebp-8h] BYREF
    int stopPos; // [esp+14h] [ebp-4h]

    iassert( IsPowerOf2( poolSize ) );
    indexMask = poolSize - 1;
    stopPos = (poolSize - 1) & (charCount + firstChar);
    parsePos = firstChar;
    width = 0;
    while (parsePos != stopPos)
    {
        letter = SEH_DecodeLetter(textPool[parsePos], textPool[indexMask & (parsePos + 1)], &usedCharCount, 0);
        parsePos = indexMask & (usedCharCount + parsePos);
        if (letter == 94
            && &textPool[parsePos]
            && textPool[parsePos] != 94
                && textPool[parsePos] >= 48
                && textPool[parsePos] <= 57)
        {
            parsePos = indexMask & (parsePos + 1);
        }
        else if (letter == 94 && (textPool[parsePos] == 1 || textPool[parsePos] == 2))
        {
            width += (font->pixelHeight * (textPool[indexMask & (parsePos + 1)] - 16) + 16) / 32;
            parsePos = indexMask & (parsePos + 7);
        }
        else
        {
            width += R_LetterWidth(letter, font);
        }
    }
    return width;
}

void __cdecl R_InitFonts()
{
    iassert( registeredFontCount == 0 );
}

void __cdecl R_ShutdownFonts()
{
    registeredFontCount = 0;
}