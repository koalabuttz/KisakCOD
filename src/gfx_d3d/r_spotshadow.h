#pragma once
#include "r_rendercmds.h"
#include "r_scene.h"

#define R_SPOTSHADOW_TILE_COUNT 4

struct GfxSpotShadowEntCmd // sizeof=0x8
{                                       // ...
    GfxSceneEntity *sceneEnt;           // ...
    const GfxLight *light;              // ...
};

void R_AddSpotShadowEntCmd(const GfxSpotShadowEntCmd *data);
char R_AddSpotShadowsForLight(
    GfxViewInfo *viewInfo,
    GfxLight *light,
    uint32_t shadowableLightIndex,
    float spotShadowFade);
void R_SetViewParmsForLight(const GfxLight *light, GfxViewParms *viewParms, float nearPlaneBias);
void R_GetSpotShadowLookupMatrix(
    const GfxViewParms *shadowViewParms,
    uint32_t spotShadowIndex,
    uint32_t tileCount,
    GfxMatrix *lookupMatrix);
void R_AddSpotShadowModelEntities(
    uint32_t localClientNum,
    uint32_t primaryLightIndex,
    const GfxLight *light);
void R_GenerateAllSortedSpotShadowDrawSurfs(GfxViewInfo *viewInfo);
void R_GenerateSortedPrimarySpotShadowDrawSurfs(
    const GfxViewInfo *viewInfo,
    uint32_t spotShadowIndex,
    uint32_t shadowableLightIndex);
void R_EmitSpotShadowMapSurfs(GfxViewInfo *viewInfo);


uint32_t R_InitSpotShadowMeshes();
void R_ShutdownSpotShadowMeshes();
void RB_SpotShadowMaps(const GfxBackEndData *data, const GfxViewInfo *viewInfo);