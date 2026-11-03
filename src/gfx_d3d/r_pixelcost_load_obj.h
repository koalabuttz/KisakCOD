#pragma once
#include <cstdint>

void R_PixelCost_PrintColorCodeKey();

void RB_PixelCost_BuildColorCodeMap(uint8_t (*pixels)[4], int pixelCount);
