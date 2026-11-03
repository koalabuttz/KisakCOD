#pragma once
#include "r_gfx.h"
#include "r_bsp.h"

#define R_MAX_PRETESS_INDICES 0x100000

struct GfxSModelDrawSurfData // sizeof=0x18
{                                       // ...
    GfxDelayedCmdBuf delayedCmdBuf;
    GfxDrawSurfList drawSurfList;       // ...
};

void R_AddDelayedStaticModelDrawSurf(
    GfxDelayedCmdBuf *delayedCmdBuf,
    struct XSurface *xsurf,
    uint8_t *list,
    uint32_t count);
void R_EndDumpStaticModelLodInfo();
void R_AddAllStaticModelSurfacesCamera();
void R_SkinStaticModelsCameraForLod(
    const XModel *model,
    uint8_t primaryLightIndex,
    uint8_t *list,
    uint32_t count,
    uint32_t surfType,
    uint32_t lod,
    GfxSModelDrawSurfLightingData *surfData);
void R_SkinStaticModelsCamera(
    const XModel *model,
    uint8_t primaryLightIndex,
    uint16_t (*staticModelLodList)[4][128],
    uint16_t (*staticModelLodCount)[4],
    GfxSModelDrawSurfLightingData *surfData);
void R_SkinStaticModelsCameraForSurface(
    const XModel *model,
    uint8_t primaryLightIndex,
    uint16_t (*staticModelLodList)[128],
    uint16_t *staticModelLodCount,
    uint32_t surfType,
    GfxSModelDrawSurfLightingData *surfData);
void R_ShowCountsStaticModel(int smodelIndex, int lod);
void R_DumpStaticModelLodInfo(const GfxStaticModelDrawInst *smodelDrawInst, float dist);
void R_StaticModelWriteInfo(int fileHandle, const GfxStaticModelDrawInst *smodelDrawInst, const float dist);
void R_SortAllStaticModelSurfacesCamera();
void R_SortAllStaticModelSurfacesSunShadow();
void R_AddAllStaticModelSurfacesSunShadow();
void R_AddAllStaticModelSurfacesRangeSunShadow(uint32_t partitionIndex, uint32_t maxDrawSurfCount);
void R_SkinStaticModelsShadowForLod(
    const XModel *model,
    uint8_t *list,
    uint32_t count,
    uint32_t surfType,
    uint32_t lod,
    GfxSModelDrawSurfData *surfData);
void R_SkinStaticModelsShadow(
    const XModel *model,
    uint16_t (*staticModelLodList)[4][128],
    uint16_t (*staticModelLodCount)[4],
    GfxSModelDrawSurfData *surfData);
void R_SkinStaticModelsShadowForSurface(
    const XModel *model,
    uint16_t (*staticModelLodList)[128],
    uint16_t *staticModelLodCount,
    uint32_t surfType,
    GfxSModelDrawSurfData *surfData);
void R_AddAllStaticModelSurfacesSpotShadow(uint32_t spotShadowIndex, uint32_t primaryLightIndex);
