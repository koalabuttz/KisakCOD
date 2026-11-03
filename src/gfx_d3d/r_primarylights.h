#pragma once
#include "r_rendercmds.h"

enum GfxLightType : __int32
{
    GFX_LIGHT_TYPE_NONE = 0x0,
    GFX_LIGHT_TYPE_DIR = 0x1,
    GFX_LIGHT_TYPE_SPOT = 0x2,
    GFX_LIGHT_TYPE_OMNI = 0x3,
    GFX_LIGHT_TYPE_COUNT = 0x4,
    GFX_LIGHT_TYPE_DIR_SHADOWMAP = 0x4,
    GFX_LIGHT_TYPE_SPOT_SHADOWMAP = 0x5,
    GFX_LIGHT_TYPE_OMNI_SHADOWMAP = 0x6,
    GFX_LIGHT_TYPE_COUNT_WITH_SHADOWMAP_VERSIONS = 0x7,
};

struct GfxCandidateShadowedLight // sizeof=0x8
{                                       // ...
    uint32_t shadowableLightIndex;  // ...
    float score;
};

struct GfxShadowedLightEntry // sizeof=0x8
{                                       // ...
    uint8_t shadowableLightIndex;
    bool isFadingOut;
    // padding byte
    // padding byte
    float fade;
};
struct GfxShadowedLightHistory // sizeof=0x48
{                                       // ...
    uint32_t shadowableLightWasUsed[8];
    GfxShadowedLightEntry entries[4];
    uint32_t entryCount;
    uint32_t lastUpdateTime;
};

struct GfxShadowGeometry // sizeof=0xC
{
    uint16_t surfaceCount;
    uint16_t smodelCount;
    uint16_t *sortedSurfIndex;
    uint16_t *smodelIndex;
};
struct GfxLightRegionAxis // sizeof=0x14
{
    float dir[3];
    float midPoint;
    float halfSize;
};
struct GfxLightRegionHull // sizeof=0x50
{
    float kdopMidPoint[9];
    float kdopHalfSize[9];
    uint32_t axisCount;
    GfxLightRegionAxis *axis;
};
struct GfxLightRegion // sizeof=0x8
{
    uint32_t hullCount;
    GfxLightRegionHull *hulls;
};

void R_ClearShadowedPrimaryLightHistory(int localClientNum);
void R_AddDynamicShadowableLight(GfxViewInfo *viewInfo, const GfxLight *visibleLight);
bool R_IsDynamicShadowedLight(uint32_t shadowableLightIndex);
bool R_IsPrimaryLight(uint32_t shadowableLightIndex);
void R_ChooseShadowedLights(GfxViewInfo *viewInfo);
uint32_t R_AddPotentiallyShadowedLight(
    const GfxViewInfo *viewInfo,
    uint32_t shadowableLightIndex,
    GfxCandidateShadowedLight *candidateLights,
    uint32_t candidateLightCount);
double R_ShadowedSpotLightScore(const GfxViewParms *viewParms, const GfxLight *light);
void R_AddShadowsForLight(GfxViewInfo *viewInfo, uint32_t shadowableLightIndex, float spotShadowFade);
void R_AddShadowedLightToShadowHistory(
    GfxShadowedLightHistory *shadowHistory,
    uint32_t shadowableLightIndex,
    float fadeDelta);
void R_FadeOutShadowHistoryEntries(GfxShadowedLightHistory *shadowHistory, float fadeDelta);
void R_LinkSphereEntityToPrimaryLights(
    uint32_t localClientNum,
    uint32_t entityNum,
    const float *origin,
    float radius);
uint32_t R_GetPrimaryLightEntityShadowBit(
    uint32_t localClientNum,
    uint32_t entnum,
    uint32_t primaryLightIndex);
void R_LinkBoxEntityToPrimaryLights(
    uint32_t localClientNum,
    uint32_t entityNum,
    const float *mins,
    const float *maxs);
char R_CullBoxFromLightRegionHull(
    const GfxLightRegionHull *hull,
    const float *boxMidPoint,
    const float *boxHalfSize);
void R_LinkDynEntToPrimaryLights(
    uint32_t dynEntId,
    DynEntityDrawType drawType,
    const float *mins,
    const float *maxs);
bool Com_CullBoxFromPrimaryLight(
    const struct ComPrimaryLight *light,
    const float *boxMidPoint,
    const float *boxHalfSize);
uint32_t R_GetPrimaryLightDynEntShadowBit(uint32_t entnum, uint32_t primaryLightIndex);
void R_UnlinkEntityFromPrimaryLights(uint32_t localClientNum, uint32_t entityNum);
void R_UnlinkDynEntFromPrimaryLights(uint32_t dynEntId, DynEntityDrawType drawType);
bool R_IsEntityVisibleToPrimaryLight(
    uint32_t localClientNum,
    uint32_t entityNum,
    uint32_t primaryLightIndex);
bool R_IsDynEntVisibleToPrimaryLight(
    uint32_t dynEntId,
    DynEntityDrawType drawType,
    uint32_t primaryLightIndex);
int R_IsEntityVisibleToAnyShadowedPrimaryLight(const GfxViewInfo *viewInfo, uint32_t entityNum);
bool R_IsEntityVisibleToShadowedPrimaryLight(uint32_t baseBitIndex, uint32_t shadowableLightIndex);
int R_IsDynEntVisibleToAnyShadowedPrimaryLight(
    const GfxViewInfo *viewInfo,
    uint32_t dynEntId,
    DynEntityDrawType drawType);
bool R_IsDynEntVisibleToShadowedPrimaryLight(
    uint32_t baseBitIndex,
    DynEntityDrawType drawType,
    uint32_t shadowableLightIndex);
uint32_t R_GetNonSunPrimaryLightForBox(
    const GfxViewInfo *viewInfo,
    const float *boxMidPoint,
    const float *boxHalfSize);
uint32_t R_GetNonSunPrimaryLightForSphere(const GfxViewInfo *viewInfo, const float *origin, float radius);
char R_CullSphereFromLightRegionHull(const GfxLightRegionHull *hull, const float *origin, float radius);
bool Com_CullSphereFromPrimaryLight(const struct ComPrimaryLight *light, const float *origin, float radius);
