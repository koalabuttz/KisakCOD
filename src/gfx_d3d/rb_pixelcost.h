#pragma once
#include "r_material.h"
#include "rb_backend.h"

enum GfxPixelCostMode : __int32
{                                       // ...
    GFX_PIXEL_COST_MODE_OFF = 0x0,
    GFX_PIXEL_COST_MODE_MEASURE_COST = 0x1,
    GFX_PIXEL_COST_MODE_MEASURE_MSEC = 0x2,
    GFX_PIXEL_COST_MODE_ADD_COST_USE_DEPTH = 0x3,
    GFX_PIXEL_COST_MODE_ADD_COST_IGNORE_DEPTH = 0x4,
    GFX_PIXEL_COST_MODE_ADD_PASSES_USE_DEPTH = 0x5,
    GFX_PIXEL_COST_MODE_ADD_PASSES_IGNORE_DEPTH = 0x6,
};


extern GfxPixelCostMode pixelCostMode;

const Material *R_PixelCost_GetAccumulationMaterial(const Material *material);
void R_PixelCost_BeginSurface(GfxCmdBufContext context);
void R_PixelCost_SetConstant(GfxCmdBufSourceState *source, int cost);
int RB_PixelCost_GetCostForRecordIndex(int recordIndex);
bool RB_PixelCost_DoesPrimMatch(const Material *material, MaterialTechniqueType techType);
void RB_PixelCost_ResetPrim(const Material *material, MaterialTechniqueType techType);
unsigned __int64 RB_PixelCost_BeginTiming();
void R_HW_FinishGpu();
void R_PixelCost_EndSurface(GfxCmdBufContext context);
int RB_PixelCost_AccumulateMsec();
void RB_PixelCost_EndTiming();
GfxRenderTargetId RB_PixelCost_OverrideRenderTarget(GfxRenderTargetId targetId);


inline bool RB_PixelCost_IsAccumulating()
{
    return pixelCostMode > GFX_PIXEL_COST_MODE_MEASURE_MSEC;
}