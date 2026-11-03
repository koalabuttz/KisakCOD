#pragma once
#include "r_gfx.h"

struct GfxStaticModelCombinedInst // sizeof=0x68
{                                       // ...
    GfxStaticModelDrawInst smodelDrawInst;
    GfxStaticModelInst smodelInst;
};

void R_StaticModelWriteInfoHeader(int fileHandle);
void R_StaticModelWriteInfo(int fileHandle, const GfxStaticModelDrawInst *smodelDrawInst, const float dist);
BOOL R_StaticModelHasLighting(uint32_t smodelIndex);
int R_StaticModelGetMemoryUsageInst();
int R_StaticModelGetMemoryUsage(XModel *model, int *modelCount);
BOOL R_StaticModelCompare(
    const GfxStaticModelCombinedInst &smodelInst0,
    const GfxStaticModelCombinedInst &smodelInst1);