#pragma once
#include "rb_backend.h"
#include "rb_tess.h"

struct GfxStaticModelDrawStream // sizeof=0x1C
{                                       // ...
    const uint32_t *primDrawSurfPos; // ...
    const GfxTexture *reflectionProbeTexture; // ...
    uint32_t customSamplerFlags;    // ...
    XSurface *localSurf;
    uint32_t smodelCount;
    const uint16_t *smodelList;
    uint32_t reflectionProbeIndex;
};

void R_DrawStaticModelSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
int R_GetNextStaticModelSurf(GfxStaticModelDrawStream *drawStream, XSurface **outSurf);
void R_DrawStaticModelSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void R_DrawStaticModelDrawSurfNonOptimized(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context);
void R_SetStaticModelVertexBuffer(GfxCmdBufPrimState *primState, XSurface *xsurf);
void R_DrawStaticModelDrawSurfPlacement(
    const GfxStaticModelDrawInst *smodelDrawInst,
    GfxCmdBufSourceState *source);
void R_DrawStaticModelDrawSurfLightingNonOptimized(
    GfxStaticModelDrawStream *drawStream,
    GfxCmdBufContext context);

void R_DrawStaticModelCachedSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void R_DrawStaticModelCachedSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void R_SetupCachedStaticModelLighting(GfxCmdBufSourceState *source);
int R_ReadStaticModelPreTessDrawSurf(
    GfxReadCmdBuf *readCmdBuf,
    GfxStaticModelPreTessSurf *pretessSurf,
    uint32_t *firstIndex,
    uint32_t *count);
void R_DrawStaticModelsPreTessDrawSurf(
    GfxStaticModelPreTessSurf pretessSurf,
    uint32_t firstIndex,
    uint32_t count,
    GfxCmdBufContext context);
void R_DrawStaticModelsPreTessDrawSurfLighting(
    GfxStaticModelPreTessSurf pretessSurf,
    uint32_t firstIndex,
    uint32_t count,
    GfxCmdBufContext context);

void R_DrawStaticModelSkinnedSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void R_DrawStaticModelSkinnedSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void R_DrawStaticModelsSkinnedDrawSurf(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context);

uint32_t R_ReadPrimDrawSurfInt(GfxReadCmdBuf *cmdBuf);
const uint32_t *R_ReadPrimDrawSurfData(GfxReadCmdBuf *cmdBuf, uint32_t count);