#pragma once
#include "rb_backend.h"
#include "r_dobj_skin.h"

struct GfxReadCmdBuf // sizeof=0x4
{                                       // ...
    const uint32_t *primDrawSurfPos; // ...
};

struct GfxStaticModelPreTessSurf_s // sizeof=0x4
{                                       // ...
    uint8_t surfIndex;
    uint8_t lod;
    uint16_t cachedIndex;
};
union GfxStaticModelPreTessSurf // sizeof=0x4
{                                       // ...
    GfxStaticModelPreTessSurf_s fields;
    uint32_t packed;
};

//union $B667868682928995E3CB40CE466D3989 // sizeof=0x4
//{                                       // ...
//    GfxPackedVertex *skinnedVert;
//    int oldSkinnedCachedOffset;
//};


void RB_ShowTess(GfxCmdBufContext context, const float *center, const char *tessName, const float *color);
uint32_t R_TessCodeMeshList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
void R_SetVertexDeclTypeNormal(GfxCmdBufState *state, MaterialVertexDeclType vertDeclType);
void R_TessCodeMeshList_AddCodeMeshArgs(
    GfxCmdBufSourceState *source,
    const GfxBackEndData *data,
    const FxCodeMeshData *codeMesh);
void R_SetParticleCloudConstants(GfxCmdBufSourceState *source, const GfxParticleCloud *cloud);
void RB_Vec3DirWorldToView(const GfxCmdBufSourceState *source, const float *worldDir, float *viewDir);
void RB_CreateParticleCloud2dAxis(const GfxParticleCloud *cloud, const float *viewUp, float (*viewAxis)[2][2]);
void R_DrawXModelSkinnedUncached(GfxCmdBufContext context, XSurface *xsurf, GfxPackedVertex *skinnedVert);
void R_DrawXModelSkinnedModelSurf(GfxCmdBufContext context, const GfxModelSkinnedSurface *modelSurf);
void R_DrawXModelSkinnedCached(GfxCmdBufContext context, const GfxModelSkinnedSurface *modelSurf);
void R_SetVertexDeclTypeWorld(GfxCmdBufState *state);

uint32_t R_TessTrianglesList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
uint32_t R_TessCodeMeshList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
uint32_t R_TessMarkMeshList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
uint32_t R_TessParticleCloudList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
uint32_t R_TessXModelSkinnedDrawSurfList(
    const GfxDrawSurfListArgs *listArgs,
    GfxCmdBufContext prepassContext);
uint32_t R_TessXModelRigidDrawSurfList(
    const GfxDrawSurfListArgs *listArgs,
    GfxCmdBufContext prepassContext);
uint32_t R_TessXModelRigidSkinnedDrawSurfList(
    const GfxDrawSurfListArgs *listArgs,
    GfxCmdBufContext prepassContext);
uint32_t R_TessStaticModelRigidDrawSurfList(
    const GfxDrawSurfListArgs *listArgs,
    GfxCmdBufContext prepassContext);
uint32_t R_TessStaticModelSkinnedDrawSurfList(
    const GfxDrawSurfListArgs *listArgs,
    GfxCmdBufContext prepassContext);
uint32_t R_TessStaticModelPreTessList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
uint32_t R_TessStaticModelCachedList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
uint32_t R_TessTrianglesPreTessList(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);
uint32_t R_TessBModel(const GfxDrawSurfListArgs *listArgs, GfxCmdBufContext prepassContext);



extern GfxScaledPlacement s_manualObjectPlacement;