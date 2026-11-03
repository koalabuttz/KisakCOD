#pragma once

struct LocalizationData // sizeof=0x8
{                                       // ...
    char* language;                     // ...
    char* strings;                      // ...
};

char* Win_CopyLocalizationString(const char* string);
char* Win_GetLanguage();
int Win_InitLocalization();
char* Win_LocalizeRef(const char* ref);
void Win_ShutdownLocalization();