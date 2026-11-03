#pragma once

#include <string>

enum msgLocErrType_t : __int32
{                                       // ...
    LOCMSG_SAFE = 0x0,
    LOCMSG_NOERR = 0x1,
};

struct languageInfo_t // sizeof=0x8
{                                       // ...
    const char *pszName;                // ...
    int bPresent;                       // ...
};

void TRACK_stringed_hooks();
int SEH_GetCurrentLanguage();
void SEH_InitLanguage();
const dvar_s *SEH_UpdateCurrentLanguage();
int SEH_VerifyLanguageSelection(int iLanguageSelection);
const char *SEH_StringEd_GetString(const char *pszReference);
char *SEH_SafeTranslateString(char *pszReference);
char *SEH_LocalizeTextMessage(const char *pszInputBuffer, const char *pszMessageType, msgLocErrType_t errType);
int SEH_GetLocalizedTokenReference(
    char *token,
    int tokenSize,
    const char *reference,
    const char *messageType,
    msgLocErrType_t errType);
bool Taiwanese_ValidBig5Code(__int16 uiCode);
bool Japanese_ValidShiftJISCode(uint32_t _iHi, uint32_t _iLo);
bool Chinese_ValidGBCode(uint8_t _iHi, uint8_t _iLo);
uint32_t SEH_DecodeLetter(
    uint32_t firstChar,
    uint32_t secondChar,
    int *usedCount,
    int *pbIsTrailingPunctuation);
bool Taiwanese_IsTrailingPunctuation(uint32_t uiCode);
bool Japanese_IsTrailingPunctuation(uint32_t uiCode);
bool Chinese_IsTrailingPunctuation(uint32_t uiCode);
uint32_t SEH_ReadCharFromString(const char **text, int *isTrailingPunctuation);
int Language_IsAsian();
int SEH_PrintStrlen(const char *string);
const char *SEH_GetLanguageName(uint32_t iLanguage);
int SEH_GetLanguageIndexForName(const char *pszLanguageName, int *piLanguageIndex);
void SEH_UpdateLanguageInfo();
void SEH_Init_StringEd();
void SEH_Shutdown_StringEd();


extern const dvar_t *loc_language;
extern const dvar_t *loc_forceEnglish;
extern const dvar_t *loc_translate;
extern const dvar_t *loc_warnings;
extern const dvar_t *loc_warningsAsErrors;