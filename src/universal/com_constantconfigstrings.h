#pragma once
#include <cstdint>

struct constantConfigString // sizeof=0x10
{
    int configStringNum;
    const char *configString;
    int configStringHash;
    int lowercaseConfigStringHash;
};

uint32_t lowercaseHash(const char *str);
void CCS_InitConstantConfigStrings();
int CCS_GetConstConfigStringIndex(const char *configString);
int CCS_GetConfigStringNumForConstIndex(uint32_t index);
uint32_t CCS_IsConfigStringIndexConstant(int index);


extern constantConfigString constantConfigStrings[833];
