#pragma once

#include <universal/layout_assert.h>
#include <cstdint>

#include <qcommon/qcommon.h>

#include <universal/memfile.h>

#include <gfx_d3d/fxprimitives.h>
#include "fx_marks.h"
#include <gfx_d3d/r_gfx.h>

#define FX_HANDLE_NONE 0xFFFF

#define FX_EFFECT_LIMIT 1024

#define GFX_ML_HANDLE_NONE 0

enum $FFE723C3A54D7F6DDF86A219D7944B2F : int32_t
{
    FX_STATUS_REF_COUNT_MASK = 0xFFFF,
    FX_STATUS_HAS_PENDING_LOOP_ELEMS = 0x10000,
    FX_STATUS_OWNED_EFFECTS_SHIFT = 0x11,
    FX_STATUS_OWNED_EFFECTS_MASK = 0x7FE0000,
    FX_STATUS_DEFER_UPDATE = 0x8000000,
    FX_STATUS_SELF_OWNED = 0x10000000,
    FX_STATUS_IS_LOCKED = 0x20000000,
    FX_STATUS_IS_LOCKED_MASK = 0x60000000,
};

enum $390C8AB619C5D27F330E671BCD9D689E : int32_t
{
    FX_ELEM_TYPE_SPRITE_BILLBOARD = 0x0,
    FX_ELEM_TYPE_SPRITE_ORIENTED = 0x1,
    FX_ELEM_TYPE_TAIL = 0x2,
    FX_ELEM_TYPE_TRAIL = 0x3,
    FX_ELEM_TYPE_CLOUD = 0x4,
    FX_ELEM_TYPE_MODEL = 0x5,
    FX_ELEM_TYPE_OMNI_LIGHT = 0x6,
    FX_ELEM_TYPE_SPOT_LIGHT = 0x7,
    FX_ELEM_TYPE_SOUND = 0x8,
    FX_ELEM_TYPE_DECAL = 0x9,
    FX_ELEM_TYPE_RUNNER = 0xA,
    FX_ELEM_TYPE_COUNT = 0xB,
    FX_ELEM_TYPE_LAST_SPRITE = 0x3,
    FX_ELEM_TYPE_LAST_DRAWN = 0x7,
};

#ifdef KISAK_SP
#define FX_BONE_INDEX_NONE 0x3FF
#else
#define FX_BONE_INDEX_NONE 0x7FF
#endif
#define FX_DOBJ_HANDLE_NONE 4095

void TRACK_fx_system();
XModel *FX_RegisterModel(const char *modelName);
FxSystem *FX_GetSystem(int32_t clientIndex);
FxSystemBuffers *FX_GetSystemBuffers(int32_t clientIndex);
void FX_LinkSystemBuffers(FxSystem *system, FxSystemBuffers *systemBuffers);
void FX_InitSystem(int32_t localClientNum);
void FX_ResetSystem(FxSystem *system);
int32_t FX_EffectToHandle(FxSystem *system, FxEffect *effect);
void FX_ShutdownSystem(int32_t localClientNum);
void FX_RelocateSystem(FxSystem *system, int32_t relocationDistance);
void FX_EffectNoLongerReferenced(FxSystem *system, FxEffect *remoteEffect);
void FX_DelRefToEffect(FxSystem *system, FxEffect *effect);
void FX_RunGarbageCollection(FxSystem *system);
bool FX_BeginIteratingOverEffects_Exclusive(FxSystem *system);
void FX_RunGarbageCollection_FreeSpotLight(FxSystem *system, uint16_t effectHandle);
void FX_RunGarbageCollection_FreeTrails(FxSystem *system, FxEffect *effect);
void FX_SpawnEffect_AllocTrails(FxSystem *system, FxEffect *effect);
FxPool<FxTrail> *FX_AllocTrail(FxSystem *system);
uint16_t FX_CalculatePackedLighting(const float *origin);
FxEffect *FX_SpawnEffect(
    FxSystem *system,
    const FxEffectDef *remoteDef,
    int32_t msecBegin,
    const float *origin,
    const float (*axis)[3],
    int32_t dobjHandle,
    int32_t boneIndex,
    int32_t runnerSortOrder,
    uint16_t owner,
    uint32_t markEntnum);
void FX_AddRefToEffect(FxSystem *__formal, FxEffect *effect);
char FX_CullEffectForSpawn(const FxCamera *camera, const FxEffectDef *effectDef, const float *origin);
bool FX_CullElemForSpawn(const FxCamera *camera, const FxElemDef *elemDef, const float *origin);
void FX_SetEffectRandomSeed(FxEffect *effect, const FxEffectDef *remoteDef);
char FX_EffectAffectsGameplay(const FxEffectDef *remoteEffectDef);
char FX_IsSpotLightEffect(FxSystem *system, const FxEffectDef *def);
bool FX_CanAllocSpotLightEffect(const FxSystem *system);
char FX_SpawnEffect_AllocSpotLightEffect(FxSystem *system, FxEffect *effect);
FxEffect *FX_SpawnOrientedEffect(
    int32_t localClientNum,
    const FxEffectDef *def,
    int32_t msecBegin,
    const float *origin,
    const float (*axis)[3],
    uint32_t markEntnum);
void FX_AssertAllocatedEffect(int32_t localClientNum, FxEffect *effect);
void FX_PlayOrientedEffectWithMarkEntity(
    int32_t localClientNum,
    const FxEffectDef *def,
    int32_t startMsec,
    const float *origin,
    const float (*axis)[3],
    uint32_t markEntnum);
void FX_PlayOrientedEffect(
    int32_t localClientNum,
    const FxEffectDef *def,
    int32_t startMsec,
    const float *origin,
    const float (*axis)[3]);
FxEffect *FX_SpawnBoltedEffect(
    int32_t localClientNum,
    const FxEffectDef *def,
    int32_t msecBegin,
    uint32_t dobjHandle,
    uint32_t boneIndex);
char FX_NeedsBoltUpdate(const FxEffectDef *def);
void FX_PlayBoltedEffect(
    int32_t localClientNum,
    const FxEffectDef *def,
    int32_t startMsec,
    uint32_t dobjHandle,
    uint32_t boneIndex);
void FX_RetriggerEffect(int32_t localClientNum, FxEffect *effect, int32_t msecBegin);
void FX_GetTrailHandleList_Last(
    FxSystem *system,
    FxEffect *effect,
    uint16_t *outHandleList,
    int32_t *outTrailCount);
void FX_ThroughWithEffect(int32_t localClientNum, FxEffect *effect);
void FX_StopEffect(FxSystem *system, FxEffect *effect);
void FX_StopEffectNonRecursive(FxSystem *system, FxEffect *effect);
void FX_KillEffect(FxSystem *system, FxEffect *effect);
void FX_RemoveAllEffectElems(FxSystem *system, FxEffect *effect);
void FX_KillEffectDef(int32_t localClientNum, const FxEffectDef *def);
void FX_KillAllEffects(int32_t localClientNum);
void FX_SpawnTrailElem_NoCull(
    FxSystem *system,
    FxEffect *effect,
    FxTrail *trail,
    const FxSpatialFrame *effectFrameWhenPlayed,
    int32_t msecWhenPlayed,
    float distanceWhenPlayed);
FxPool<FxTrailElem> *FX_AllocTrailElem(FxSystem *system);
void FX_SpawnTrailElem_Cull(
    FxSystem *system,
    FxEffect *effect,
    FxTrail *trail,
    const FxSpatialFrame *effectFrameWhenPlayed,
    int32_t msecWhenPlayed,
    float distanceWhenPlayed);
bool FX_CullTrailElem(
    const FxCamera *camera,
    const FxElemDef *elemDef,
    const float *origin,
    uint8_t sequence);
void FX_SpawnSpotLightElem(FxSystem *system, FxElem *elem);
void FX_SpawnElem(
    FxSystem *system,
    FxEffect *effect,
    int32_t elemDefIndex,
    const FxSpatialFrame *effectFrameWhenPlayed,
    int32_t msecWhenPlayed,
    float distanceWhenPlayed,
    int32_t sequence);
FxPool<FxElem> *FX_AllocElem(FxSystem *system);
void FX_SpawnRunner(
    FxSystem *system,
    FxEffect *effect,
    const FxElemDef *remoteElemDef,
    const FxSpatialFrame *effectFrameWhenPlayed,
    int32_t randomSeed,
    int32_t msecWhenPlayed);
bool FX_SpawnModelPhysics(
    FxSystem *system,
    FxEffect *effect,
    const FxElemDef *elemDef,
    int32_t randomSeed,
    FxElem *elem);
void FX_GetOriginForElem(
    FxEffect *effect,
    const FxElemDef *elemDef,
    const FxSpatialFrame *effectFrameWhenPlayed,
    int32_t randomSeed,
    float *outOrigin);
void FX_SpawnSound(
    int32_t localClientNumber,
    FxEffect *effect,
    const FxElemDef *elemDef,
    const FxSpatialFrame *effectFrameWhenPlayed,
    int32_t randomSeed);
void FX_FreeElem(FxSystem *system, uint16_t elemHandle, FxEffect *effect, uint32_t elemClass);
void FX_FreeTrailElem(FxSystem *system, uint16_t trailElemHandle, FxEffect *effect, FxTrail *trail);
void FX_FreeSpotLightElem(FxSystem *system, uint16_t elemHandle, FxEffect *effect);
double FX_GetClientVisibility(int32_t localClientNum, const float *start, const float *end);
void FX_TrailElem_CompressBasis(const float (*inBasis)[3], char (*outBasis)[3]);

double FX_GetServerVisibility(const float *start, const float *end);
FxEffect *FX_GetClientEffectByIndex(int clientIndex, uint32_t index);
int FX_GetClientEffectIndex(int clientIndex, FxEffect *effect);

extern FxSystem fx_systemPool[1];
extern FxSystemBuffers fx_systemBufferPool[1];


// fx_Dvars
void FX_RegisterDvars(void);

extern const dvar_t *fx_mark_profile;
extern const dvar_t *fx_marks_smodels;
extern const dvar_t *fx_enable;
extern const dvar_t *fx_cull_effect_spawn;
extern const dvar_t *fx_cull_elem_draw;
extern const dvar_t *fx_freeze;
extern const dvar_t *fx_debugBolt;
extern const dvar_t *fx_count;
extern const dvar_t *fx_visMinTraceDist;
extern const dvar_t *fx_drawClouds;
extern const dvar_t *fx_marks;
extern const dvar_t *fx_draw;
extern const dvar_t *fx_profile;
extern const dvar_t *fx_cull_elem_spawn;
extern const dvar_t *fx_marks_ents;

struct cpose_t;

// fx_marks
enum MarkFragmentsAgainstEnum : int32_t
{                                       // ...
    MARK_FRAGMENTS_AGAINST_BRUSHES = 0x0,
    MARK_FRAGMENTS_AGAINST_MODELS = 0x1,
};
struct FxMarkTri // sizeof=0xC
{                                       // ...
    uint16_t indices[3];        // ...
    GfxMarkContext context;             // ...
};
static_assert(sizeof(FxMarkTri) == 0xC);

struct MarkInfoCollidedDObj // sizeof=0xC
{                                       // ...
    DObj_s *dObj;
    cpose_t *pose;
    uint16_t entnum;
    // padding byte
    // padding byte
};
static_assert(LAYOUT_32BIT(sizeof(MarkInfoCollidedDObj) == 0xC));

struct MarkInfoCollidedBModel // sizeof=0xC
{                                       // ...
    GfxBrushModel *brushModel;
    cpose_t *pose;
    uint16_t entnum;
    // padding byte
    // padding byte
};
static_assert(LAYOUT_32BIT(sizeof(MarkInfoCollidedBModel) == 0xC));

struct MarkInfo // sizeof=0x448
{                                       // ...
    float origin[3];
    float localOrigin[3];
    float localTexCoordAxis[3];
    float axis[3][3];
    float radius;
    Material *material;
    int32_t maxTris;
    FxMarkTri *tris;
    int32_t maxPoints;
    FxMarkPoint *points;
    float mins[3];
    float maxs[3];
    float planes[6][4];
    float viewOffset[3];
    bool markHasLightmap;
    bool markHasReflection;
    // padding byte
    // padding byte
    MarkFragmentsAgainstEnum markAgainst;
    uint16_t smodelsCollided[32];
    int32_t smodelCollidedCount;
    MarkInfoCollidedDObj sceneDObjsCollided[32];
    int32_t sceneDObjCollidedCount;
    MarkInfoCollidedBModel sceneBModelsCollided[32];
    int32_t sceneBModelCollidedCount;
    int32_t usedTriCount;
    int32_t usedPointCount;
    void(*callback)(void *, int32_t, FxMarkTri *, int32_t, FxMarkPoint *, const float *, const float *);
    void *callbackContext;
};
static_assert(LAYOUT_32BIT(sizeof(MarkInfo) == 0x448));

struct MarkModelCoreContext // sizeof=0x1C
{                                       // ...
    MarkInfo *markInfo;                 // ...
    GfxMarkContext *markContext;        // ...
    const float *markOrigin;            // ...
    const float *markDir;               // ...
    const float (*clipPlanes)[4];       // ...
    const float (*transformMatrix)[3];  // ...
    const float (*transformNormalMatrix)[3]; // ...
};
static_assert(LAYOUT_32BIT(sizeof(MarkModelCoreContext) == 0x1C));

struct FxMarkDObjUpdateContext // sizeof=0x108
{                                       // ...
    XModel *models[32];
    const char *modelParentBones[32];
    int32_t modelCount;
    bool isBrush;
    // padding byte
    uint16_t brushIndex;
};
static_assert(LAYOUT_32BIT(sizeof(FxMarkDObjUpdateContext) == 0x108));

struct FxActiveMarkSurf // sizeof=0x14
{                                       // ...
    Material *material;
    GfxMarkContext context;
    // padding byte
    // padding byte
    int32_t indexCount;
    uint16_t *indices;
};
static_assert(LAYOUT_32BIT(sizeof(FxActiveMarkSurf) == 0x14));

void TRACK_fx_marks();
void FX_InitMarksSystem(FxMarksSystem *marksSystem);
uint16_t FX_MarkToHandle(FxMarksSystem *marksSystem, FxMark *mark);
void FX_BeginMarks(int32_t clientIndex);
void FX_CreateImpactMark(
    int32_t localClientNum,
    const FxElemDef *elemDef,
    const FxSpatialFrame *spatialFrame,
    int32_t randomSeed,
    uint32_t markEntnum);
FxMark *FX_MarkFromHandle(FxMarksSystem *marksSystem, uint16_t handle);
void FX_MarkEntDetachAll(int32_t localClientNum, int32_t entnum);
void FX_MarkEntUpdateHidePartBits(
    const uint32_t *oldHidePartBits,
    const uint32_t *newHidePartBits,
    int32_t localClientNum,
    int32_t entnum);
void FX_MarkEntUpdateBegin(
    FxMarkDObjUpdateContext *context,
    DObj_s *obj,
    bool isBrush,
    uint16_t brushIndex);
void FX_MarkEntUpdateEnd(
    FxMarkDObjUpdateContext *context,
    int32_t localClientNum,
    int32_t entnum,
    DObj_s *obj,
    bool isBrush,
    uint16_t brushIndex);
void FX_BeginGeneratingMarkVertsForEntModels(int32_t localClientNum, uint32_t *indexCount);
void FX_GenerateMarkVertsForEntXModel(
    int32_t localClientNum,
    int32_t entId,
    uint32_t *indexCount,
    uint16_t lightHandle,
    uint8_t reflectionProbeIndex,
    const GfxScaledPlacement *placement);
void FX_GenerateMarkVertsForEntDObj(
    int32_t localClientNum,
    int32_t entId,
    uint32_t *indexCount,
    uint16_t lightHandle,
    uint8_t reflectionProbeIndex,
    const DObj_s *dobj,
    const cpose_t *pose);
void FX_GenerateMarkVertsForEntBrush(
    int32_t localClientNum,
    int32_t entId,
    uint32_t *indexCount,
    uint8_t reflectionProbeIndex,
    const GfxPlacement *placement);
void FX_EndGeneratingMarkVertsForEntModels(int32_t localClientNum);
void FX_GenerateMarkVertsForStaticModels(
    int32_t localClientNum,
    int32_t smodelCount,
    const uint8_t *smodelVisLods);
void FX_GenerateMarkVertsForWorld(int32_t localClientNum);




// fx_Draw
enum FxRandKey : int32_t
{                                       // ...
    FXRAND_VELOCITY_X = 0x0,
    FXRAND_VELOCITY_Y = 0x1,
    FXRAND_VELOCITY_Z = 0x2,
    FXRAND_ANGULAR_VELOCITY_PITCH = 0x3,
    FXRAND_ANGULAR_VELOCITY_YAW = 0x4,
    FXRAND_ANGULAR_VELOCITY_ROLL = 0x5,
    FXRAND_ORIGIN_X = 0x6,
    FXRAND_ORIGIN_Y = 0x7,
    FXRAND_ORIGIN_Z = 0x8,
    FXRAND_OFFSET_YAW = 0x9,
    FXRAND_OFFSET_HEIGHT = 0xA,
    FXRAND_OFFSET_RADIUS = 0xB,
    FXRAND_ANGLES_PITCH = 0xC,
    FXRAND_ANGLES_YAW = 0xD,
    FXRAND_ANGLES_ROLL = 0xE,
    FXRAND_GRAVITY = 0xF,
    FXRAND_REFLECTION_FACTOR = 0x10,
    FXRAND_LIFE_SPAN = 0x11,
    FXRAND_SPAWN_DELAY = 0x12,
    FXRAND_SPAWN_COUNT = 0x13,
    FXRAND_EMIT_DIST = 0x14,
    FXRAND_VISUAL = 0x15,
    FXRAND_TILE_START = 0x16,
    FXRAND_COLOR = 0x17,
    FXRAND_ROTATION = 0x18,
    FXRAND_ROTATION_DELTA = 0x19,
    FXRAND_SIZE_0 = 0x1A,
    FXRAND_SIZE_1 = 0x1B,
    FXRAND_SCALE = 0x1C,
    FXRAND_COUNT = 0x1D,
};

struct FxDrawState // sizeof=0xA8
{                                       // ...
    FxSystem *system;             // ...
    const FxEffect *effect;             // ...
    const FxElem *elem;
    const FxElemDef *elemDef;
    orientation_t orient;
    FxCamera *camera;
    int32_t randomSeed;
    float msecLifeSpan;
    float msecElapsed;
    float normTimeUpdateEnd;
    float posWorld[3];
    float velDirWorld[3];
    FxElemVisualState visState;
    FxElemPreVisualState preVisState;
    float physicsLerpFrac;
    int32_t msecDraw;                       // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxDrawState) == 0xA8));

struct FxTrailSegmentDrawState // sizeof=0x3C
{                                       // ...
    FxTrailDef *trailDef;
    float posWorld[3];                  // ...
    float basis[2][3];                  // ...
    float rotation;
    float size[2];
    float uCoord;                       // ...
    uint8_t color[4];           // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxTrailSegmentDrawState) == 0x3C));

struct FxBeam // sizeof=0x34
{                                       // ...
    float begin[3];                     // ...
    float end[3];                       // ...
    GfxColor beginColor;                // ...
    GfxColor endColor;                  // ...
    float beginRadius;                  // ...
    float endRadius;                    // ...
    Material *material;                 // ...
    int32_t segmentCount;                   // ...
    float wiggleDist;                   // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxBeam) == 0x34));

struct FxBeamInfo // sizeof=0x1384
{                                       // ...
    FxBeam beams[96];
    int32_t beamCount;                      // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxBeamInfo) == 0x1384));

struct FxPostLight // sizeof=0x24
{                                       // ...
    float begin[3];                     // ...
    float end[3];                       // ...
    float radius;                       // ...
    GfxColor color;                     // ...
    Material *material;                 // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxPostLight) == 0x24));

struct FxPostLightInfo // sizeof=0xD84
{                                       // ...
    FxPostLight postLights[96];
    int32_t postLightCount;                 // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxPostLightInfo) == 0xD84));

struct FxGenerateVertsCmd // sizeof=0x44
{                                       // ...
    FxSystem *system;
    FxBeamInfo *beamInfo;
    FxPostLightInfo *postLightInfo;
    FxSpriteInfo *spriteInfo;
    int32_t localClientNum;
    float vieworg[3];
    float viewaxis[3][3];
};
static_assert(LAYOUT_32BIT(sizeof(FxGenerateVertsCmd) == 0x44));

void FX_EvaluateVisAlpha(FxElemPreVisualState *preVisState, FxElemVisualState *visState);
uint8_t FX_InterpolateColor(
    const FxElemVisStateSample *refState,
    float valueLerp,
    float valueLerpInv,
    float sampleLerp,
    float sampleLerpInv,
    int32_t channel);
void FX_SetupVisualState(
    const FxElemDef *elemDef,
    const FxEffect *effect,
    int32_t randomSeed,
    float normTimeUpdateEnd,
    FxElemPreVisualState *preVisState);
void FX_EvaluateSize(FxElemPreVisualState *preVisState, FxElemVisualState *visState);
double FX_InterpolateSize(
    const FxElemVisStateSample *refState,
    int32_t randomSeed,
    FxRandKey randomKey,
    float sampleLerp,
    float sampleLerpInv,
    int32_t channel);
void FX_EvaluateVisualState(FxElemPreVisualState *preVisState, float msecLifeSpan, FxElemVisualState *visState);
double FX_IntegrateRotationFromZero(
    const FxElemVisStateSample *refState,
    int32_t randomSeed,
    FxRandKey randomKey,
    float sampleLerp,
    float msecLifeSpan);
void FX_EvaluateVisualState_DoLighting(
    FxElemPreVisualState *preVisState,
    FxElemVisualState *visState,
    const FxElemDef *elemDef);
void FX_UnpackColor565(
    uint16_t packed,
    uint8_t *outR,
    uint8_t *outG,
    uint8_t *outB);
void FX_DrawElem_BillboardSprite(FxDrawState *draw);
void FX_GenSpriteVerts(FxDrawState *draw, const float *tangent, const float *binormal, const float *normal);
void FX_GetSpriteTexCoords(const FxDrawState *draw, float *s0, float *ds, float *t0, float *dt);
bool FX_CullElementForDraw_Sprite(const FxDrawState *draw);
uint32_t FX_CullElementForDraw_FrustumPlaneCount(const FxDrawState *draw);
void FX_DrawElem_OrientedSprite(FxDrawState *draw);
void FX_DrawElem_Tail(FxDrawState *draw);
bool FX_CullElementForDraw_Tail(const FxDrawState *draw);
char FX_CullCylinder(
    const FxCamera *camera,
    uint32_t frustumPlaneCount,
    const float *posWorld0,
    const float *posWorld1,
    float radius);
void FX_DrawElem_Cloud(FxDrawState *draw);
void FX_SetPlacement(const FxDrawState *draw, GfxScaledPlacement *placement);
double FX_GetMsecForSamplingAxis(float msecElapsed, float msecLifeSpan, int32_t atRestFraction);
double FX_InterpolateScale(
    const FxElemVisStateSample *refState,
    int32_t randomSeed,
    FxRandKey randomKey,
    float sampleLerp,
    float sampleLerpInv);
bool FX_CullElementForDraw_Cloud(const FxDrawState *draw);
void FX_DrawElem_Model(FxDrawState *draw);
void FX_SetPlacementFromPhysics(const FxDrawState *draw, GfxPlacement *placement);
void FX_DrawElem_Light(FxDrawState *draw);
bool FX_CullElementForDraw_Light(const FxDrawState *draw);
void FX_DrawElem_SpotLight(FxDrawState *draw);
void FX_DrawNonSpriteElems(FxSystem *system);
void FX_BeginIteratingOverEffects_Cooperative(FxSystem *system);
void FX_DrawNonSpriteEffect(FxSystem *system, FxEffect *effect, uint32_t elemClass, int32_t drawTime);
void FX_DrawElement(FxSystem *system, const FxElemDef *elemDef, const FxElem *elem, FxDrawState *draw);
void FX_DrawSpotLight(FxSystem *system);
void FX_DrawSpotLightEffect(FxSystem *system, FxEffect *effect, int32_t drawTime);
void FX_DrawSpriteElems(FxSystem *system, int32_t drawTime);
void FX_DrawTrailsForEffect(FxSystem *system, FxEffect *effect, int32_t drawTime);
void FX_DrawTrail(FxSystem *system, FxDrawState *draw, FxTrail *trail);
void FX_TrailElem_UncompressBasis(const char (*inBasis)[3], float (*basis)[3]);
void FX_GenTrail_IndsForSegment(
    FxDrawState *draw,
    uint16_t reservedBaseVertex,
    r_double_index_t *outIndices);
void Fx_GenTrail_PopulateSegmentDrawState(
    FxDrawState *draw,
    float spawnDist,
    float uCoordOffset,
    const float (*basis)[3],
    FxTrailSegmentDrawState *outState);
void FX_GenTrail_VertsForSegment(const FxTrailSegmentDrawState *segmentDrawState, GfxPackedVertex *remoteVerts);
void Vec3MadMad(
    const float *start,
    float scale0,
    const float *dir0,
    float scale1,
    const float *dir1,
    float *result);
void FX_DrawSpriteEffect(FxSystem *system, FxEffect *effect, int32_t drawTime);
void FX_GenerateVerts(FxGenerateVertsCmd *cmd);
void FX_FillGenerateVertsCmd(int32_t localClientNum, FxGenerateVertsCmd *cmd);
void FX_EvaluateDistanceFade(FxDrawState *draw);
double FX_ClampRangeLerp(float dist, const FxFloatRange *range);
void FX_DrawElement_Setup_1_(
    FxSystem* system,
    FxDrawState* draw,
    int32_t elemMsecBegin,
    int32_t elemSequence,
    const float* elemOrigin,
    float* outRealNormTime);

// fx_update_util
void FX_OffsetSpawnOrigin(
    const FxSpatialFrame *effectFrame,
    const FxElemDef *elemDef,
    int32_t randomSeed,
    float *spawnOrigin);
void FX_GetOriginForTrailElem(
    FxEffect *effect,
    const FxElemDef *elemDef,
    const FxSpatialFrame *effectFrameWhenPlayed,
    int32_t randomSeed,
    float *outOrigin,
    float *outRight,
    float *outUp);
void FX_GetSpawnOrigin(
    const FxSpatialFrame *effectFrame,
    const FxElemDef *elemDef,
    int32_t randomSeed,
    float *spawnOrigin);
void FX_TransformPosFromLocalToWorld(const FxSpatialFrame *frame, float *posLocal, float *posWorld);
void FX_SpatialFrameToOrientation(const FxSpatialFrame *frame, orientation_t *orient);
void FX_OrientationDirToWorldDir(const orientation_t *orient, const float *dir, float *out);
void FX_GetOrientation(
    const FxElemDef* elemDef,
    const FxSpatialFrame* frameAtSpawn,
    const FxSpatialFrame* frameNow,
    int32_t randomSeed,
    orientation_t* orient);
char  FX_GenerateBeam_GetFlatDelta(
    const float4x4* clipMtx,
    const float4x4* invClipMtx,
    float4 beamWorldBegin,
    float4 beamWorldEnd,
    float4* outFlatDelta);
void FX_GetVelocityAtTime(
    const FxElemDef *elemDef,
    int32_t randomSeed,
    float msecLifeSpan,
    float msecElapsed,
    const orientation_t *orient,
    const float *baseVel,
    float *velocity);
void FX_GetVelocityAtTimeInFrame(
    const FxElemVelStateInFrame *statePrev,
    const FxElemVelStateInFrame *stateNext,
    const float *rangeLerp,
    const float *weight,
    float *velocity);
void FX_OrientationPosToWorldPos(const orientation_t *orient, const float *pos, float *out);
void FX_OrientationPosFromWorldPos(const orientation_t *orient, const float *pos, float *out);
void FX_AddVisBlocker(FxSystem *system, const float *posWorld, float radius, float opacity);
void FX_ToggleVisBlockerFrame(FxSystem *system);
char FX_CullSphere(const FxCamera *camera, uint32_t frustumPlaneCount, const float *posWorld, float radius);
void FX_GetElemAxis(
    const FxElemDef *elemDef,
    int32_t randomSeed,
    const orientation_t *orient,
    float msecElapsed,
    mat3x3& axis);
void FX_AnglesToOrientedAxis(const float *anglesInRad, const orientation_t *orient, float (*axisOut)[3][3]);


// fx_random
void TRACK_fx_random();
void FX_RandomDir(int32_t seed, float *dir);
void FX_RandomlyRotateAxis(const float (*axisIn)[3], int32_t randomSeed, mat3x3& axisOut);



// fx_sort
struct FxInsertSortElem // sizeof=0x14
{                                       // ...
    int32_t defSortOrder;                   // ...
    float distToCamSq;
    int32_t msecBegin;
    int32_t defIndex;
    uint8_t elemType;
    // padding byte
    // padding byte
    // padding byte
};
static_assert(sizeof(FxInsertSortElem) == 0x14);

void FX_SortEffects(FxSystem *system);
void FX_WaitBeginIteratingOverEffects_Exclusive(FxSystem *system);
bool FX_FirstEffectIsFurther(FxEffect *firstEffect, FxEffect *secondEffect);
int32_t FX_CalcRunnerParentSortOrder(FxEffect *effect);
void FX_SortNewElemsInEffect(FxSystem *system, FxEffect *effect);
void FX_SortSpriteElemIntoEffect(FxSystem *system, FxEffect *effect, FxElem *elem);
void FX_GetInsertSortElem(
    const FxSystem *system,
    const FxEffect *effect,
    const FxElem *elem,
    FxInsertSortElem *sortElem);
bool FX_ExistingElemSortsBeforeNewElem(
    const FxSystem *system,
    const FxEffect *effect,
    const FxElem *elem,
    const FxInsertSortElem *sortElemNew);


// fx_archive
struct FxEffectDefTableEntry // sizeof=0x8
{                                       // ...
    uint32_t key;
    const FxEffectDef *effectDef;
};
static_assert(LAYOUT_32BIT(sizeof(FxEffectDefTableEntry) == 0x8));

struct FxEffectDefTable // sizeof=0x2004
{                                       // ...
    int32_t count;
    FxEffectDefTableEntry entries[1024];
};
static_assert(LAYOUT_32BIT(sizeof(FxEffectDefTable) == 0x2004));

void FX_Restore(int32_t clientIndex, MemoryFile *memFile);
void FX_RestoreEffectDefTable(MemoryFile *memFile, FxEffectDefTable *table);
void FX_AddEffectDefTableEntry(FxEffectDefTable *table, uint32_t key, const FxEffectDef *effectDef);
void FX_FixupEffectDefHandles(FxSystem *system, FxEffectDefTable *table);
FxEffect *FX_EffectFromHandle(FxSystem *system, uint16_t handle);
const FxEffectDef *FX_FindEffectDefInTable(const FxEffectDefTable *table, uint32_t key);
void FX_RestorePhysicsData(FxSystem *system, MemoryFile *memFile);
FxElemVisuals FX_GetElemVisuals(const FxElemDef *elemDef, int32_t randomSeed);
void FX_Save(int32_t clientIndex, MemoryFile *memFile);
void FX_SaveEffectDefTable(FxSystem *system, MemoryFile *memFile);
void FX_SaveEffectDefTable_FastFile(MemoryFile *memFile);
void FX_SaveEffectDefTable_LoadObj(MemoryFile* memFile);
void FX_SavePhysicsData(FxSystem *system, MemoryFile *memFile);
void FX_Archive(int32_t clientIndex, MemoryFile *memFile);

// fx_beam
void FX_Beam_GenerateVerts(FxGenerateVertsCmd *cmd);

void FX_Beam_Begin();
void FX_Beam_Add(FxBeam *beam);
FxBeamInfo *FX_Beam_GetInfo();


// fx_postlight
void FX_PostLight_GenerateVerts(FxPostLightInfo *postLightInfoAddr, FxSystem *system);
void FX_PostLight_Begin();
void FX_PostLight_Add(FxPostLight *postLight);
FxPostLightInfo *FX_PostLight_GetInfo();


// fx_profile
struct FxProfileEntry // sizeof=0x1C
{                                       // ...
    const FxEffectDef *effectDef;
    int32_t effectCount;
    int32_t activeElemCount;
    int32_t pendingElemCount;
    int32_t trailCount;
    int32_t activeTrailElemCount;
    int32_t pendingTrailElemCount;
};
static_assert(LAYOUT_32BIT(sizeof(FxProfileEntry) == 0x1C));

void FX_DrawProfile(int32_t clientIndex, void(*drawFunc)(char *), float *profilePos);
FxProfileEntry *FX_GetProfileEntry(const FxEffectDef *effectDef, FxProfileEntry *entryPool, int32_t *entryCount);
void FX_ProfileSingleEffect(FxSystem *system, const FxEffect *effect, FxProfileEntry *entry);
int32_t FX_CompareProfileEntries(const FxProfileEntry *e0, const FxProfileEntry *e1);
double FX_GetProfileEntryCost(const FxProfileEntry *entry);
void FX_DrawMarkProfile(int32_t clientIndex, void(*drawFunc)(const char *, float *), float *profilePos);
void FX_DrawMarkProfile_MarkPrint(
    FxMarksSystem* marksSystem,
    uint16_t head,
    const char* name,
    int32_t index,
    void(* drawFunc)(const char*, float*),
    float* profilePos);



// fx_sprite
struct FxSprite // sizeof=0x20
{                                       // ...
    Material *material;
    float pos[3];
    uint8_t rgbaColor[4];
    float radius;
    float minScreenRadius;
    int32_t flags;
};
static_assert(LAYOUT_32BIT(sizeof(FxSprite) == 0x20));

void FX_SpriteGenerateVerts(FxGenerateVertsCmd *cmd);
void FX_GenerateSpriteCodeMeshVerts(FxSprite *sprite, FxGenerateVertsCmd *cmd);
void FX_GenerateSpriteCodeMeshVertsFixedScreenSize(
    Material *material,
    const float *pos,
    float radius,
    const uint8_t *rgbaColor,
    char spriteFlags,
    FxGenerateVertsCmd *cmd);
void FX_BuildSpriteCodeMeshVerts(
    Material *material,
    const float *pos,
    float worldRadius,
    const uint8_t *rgbaColor,
    char spriteFlags);
void FX_BuildQuadStampCodeMeshVerts(
    Material *material,
    const float *viewAxis,
    const float *origin,
    const float *left,
    const float *up,
    const uint8_t *rgbaColor,
    int32_t s0,
    int32_t t0,
    int32_t s1,
    int32_t t1);
char FX_HeightScreenToWorld(
    const float *worldOrigin,
    float screenHeight,
    float *worldHeight,
    FxGenerateVertsCmd *cmd);
double FX_GetClipSpaceW(const float *worldPoint, float *vieworg, float (*viewaxis)[3]);
void FX_GenerateSpriteCodeMeshVertsFixedWorldSize(
    Material* material,
    const float* pos,
    float radius,
    float minScreenRadius,
    const uint8_t* rgbaColor,
    char spriteFlags,
    FxGenerateVertsCmd* cmd);
char FX_HeightWorldToScreen(
    const float *worldOrigin,
    float worldHeight,
    float *screenHeight,
    FxGenerateVertsCmd *cmd);
void FX_SpriteBegin();
void FX_SpriteAdd(FxSprite *sprite);
FxSpriteInfo *FX_SpriteGetInfo();


// fx_update
enum FxUpdateResult : int32_t
{                                       // ...
    FX_UPDATE_REMOVE = 0x0,
    FX_UPDATE_KEEP = 0x1,
};

void FX_SpawnAllFutureLooping(
    FxSystem *system,
    FxEffect *effect,
    int32_t elemDefFirst,
    int32_t elemDefCount,
    const FxSpatialFrame *frameBegin,
    const FxSpatialFrame *frameEnd,
    int msecWhenPlayed,
    int mescUpdateBegin);
void FX_SpawnLoopingElems(
    FxSystem *system,
    FxEffect *effect,
    int32_t elemDefIndex,
    const FxSpatialFrame *frameBegin,
    const FxSpatialFrame *frameEnd,
    int msecWhenPlayed,
    int msecUpdateBegin,
    int msecUpdateEnd);
int32_t FX_LimitStabilizeTimeForElemDef_Recurse(
    const FxElemDef *elemDef,
    bool needToSpawnSystem,
    int32_t originalUpdateTime);
int32_t FX_LimitStabilizeTimeForElemDef_SelfOnly(const FxElemDef *elemDef, bool needToSpawnSystem);
int32_t FX_LimitStabilizeTimeForEffectDef_Recurse(const FxEffectDef *remoteEffectDef, int32_t originalUpdateTime);
void FX_BeginLooping(
    FxSystem* system,
    FxEffect* effect,
    int32_t elemDefFirst,
    int32_t elemDefCount,
    FxSpatialFrame* frameWhenPlayed,
    FxSpatialFrame* a2,
    int32_t msecWhenPlayed,
    int32_t msecNow);
void FX_SpawnTrailLoopingElems(
    FxSystem* system,
    FxEffect* effect,
    FxTrail* trail,
    FxSpatialFrame* frameBegin,
    FxSpatialFrame* frameEnd,
    int32_t msecWhenPlayed,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    float distanceTravelledBegin,
    float distanceTravelledEnd);
void FX_TriggerOneShot(
    FxSystem *system,
    FxEffect *effect,
    int32_t elemDefFirst,
    int32_t elemDefCount,
    const FxSpatialFrame *frameWhenPlayed,
    int32_t msecWhenPlayed);
void FX_SpawnOneShotElems(
    FxSystem *system,
    FxEffect *effect,
    int32_t elemDefIndex,
    const FxSpatialFrame *frameWhenPlayed,
    int32_t msecWhenPlayed);
void FX_StartNewEffect(FxSystem* system, FxEffect* effect);
bool FX_GetBoltTemporalBits(int32_t localClientNum, int32_t dobjHandle);
char FX_GetBoneOrientation(int32_t localClientNum, uint32_t dobjHandle, int32_t boneIndex, orientation_t *orient);
bool FX_GetBoneOrientation_IsDObjEntityValid(int32_t localClientNum, int32_t dobjHandle);
void FX_UpdateEffectPartial(
    FxSystem* system,
    FxEffect* effect,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    float distanceTravelledBegin,
    float distanceTravelledEnd,
    uint16_t* elemHandleStart,
    uint16_t* elemHandleStop,
    uint16_t* trailElemStart,
    uint16_t* trailElemStop);
void FX_ProcessLooping(
    FxSystem* system,
    FxEffect* effect,
    int32_t elemDefFirst,
    int32_t elemDefCount,
    FxSpatialFrame* frameBegin,
    FxSpatialFrame* frameEnd,
    int32_t msecWhenPlayed,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    float distanceTravelledBegin,
    float distanceTravelledEnd);
void FX_UpdateEffectPartialForClass(
    FxSystem *system,
    FxEffect *effect,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    uint16_t elemHandleStart,
    uint16_t elemHandleStop,
    uint32_t elemClass);
FxUpdateResult FX_UpdateElement(
    FxSystem *system,
    FxEffect *effect,
    FxElem *elem,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd);
const FxElemDef *FX_GetUpdateElemDef(const FxUpdateElem *update);
double FX_GetAtRestFraction(const FxUpdateElem *update, float msec);
int32_t FX_UpdateElementPosition(FxSystem *system, FxUpdateElem *update);
int32_t FX_UpdateElementPosition_Colliding(FxSystem *system, FxUpdateElem *update);
int32_t FX_UpdateElementPosition_CollidingStep(
    FxSystem *system,
    FxUpdateElem *update,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    float *xyzWorldOld);
void FX_NextElementPosition(FxUpdateElem *update, int32_t msecUpdateBegin, int32_t msecUpdateEnd);
void FX_NextElementPosition_NoExternalForces(
    FxUpdateElem* update,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    float* posLocal,
    float* posWorld);
void FX_IntegrateVelocity(const FxUpdateElem *update, float t0, float t1, float *posLocal, float *posWorld);
void FX_IntegrateVelocityAcrossSegments(
    int32_t elemDefFlags,
    const orientation_t *orient,
    const FxElemVelStateSample *velState0,
    const FxElemVelStateSample *velState1,
    float t0,
    float t1,
    const float *amplitudeScale,
    float integralScale,
    float *posLocal,
    float *posWorld);
void FX_IntegrateVelocityFromZeroInSegment(
    const FxElemVelStateInFrame *statePrev,
    const FxElemVelStateInFrame *stateNext,
    float *weight,
    const float *amplitudeScale,
    float integralScale,
    float *pos);
void FX_IntegrateVelocityInSegment(
    int32_t elemDefFlags,
    const orientation_t *orient,
    const FxElemVelStateSample *velState,
    float t0,
    float t1,
    const float *amplitudeScale,
    float integralScale,
    float *posLocal,
    float *posWorld);
void FX_IntegrateVelocityInSegmentInFrame(
    const FxElemVelStateInFrame *statePrev,
    const FxElemVelStateInFrame *stateNext,
    const float *weight,
    const float *amplitudeScale,
    float integralScale,
    float *pos);
bool FX_TraceHitSomething(const trace_t *trace);
int32_t FX_CollisionResponse(
    FxSystem *system,
    FxUpdateElem *update,
    const trace_t *trace,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    float *xyzWorldOld);
void FX_SpawnImpactEffect(
    FxSystem *system,
    const FxUpdateElem *update,
    const FxEffectDef *impactEffect,
    int32_t msecOnImpact,
    const float *impactNormal);
int32_t FX_UpdateElementPosition_NonColliding(FxUpdateElem *update);
int32_t FX_UpdateElementPosition_Local(FxUpdateElem *update);
void FX_SpawnDeathEffect(FxSystem *system, FxUpdateElem *update);
char FX_UpdateElement_SetupUpdate(
    FxEffect *effect,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    uint32_t elemDefIndex,
    int32_t elemAtRestFraction,
    int32_t elemMsecBegin,
    int32_t elemSequence,
    float *elemOrigin,
    FxUpdateElem *update);
void FX_UpdateElement_TruncateToElemEnd(FxUpdateElem *update, FxUpdateResult *outUpdateResult);
void FX_UpdateElement_HandleEmitting(
    FxSystem *system,
    FxElem *elem,
    FxUpdateElem *update,
    const float *elemOriginPrev,
    FxUpdateResult *outUpdateResult);
uint8_t FX_ProcessEmitting(
    FxSystem *system,
    FxUpdateElem *update,
    uint8_t emitResidual,
    FxSpatialFrame* frameBegin,
    FxSpatialFrame* frameEnd);
void FX_GetQuatForOrientation(
    const FxEffect *effect,
    const FxElemDef *elemDef,
    const FxSpatialFrame *frameNow,
    orientation_t *orient,
    float *quat);
char FX_UpdateElement_TruncateToElemBegin(FxUpdateElem *update, FxUpdateResult *outUpdateResult);
void FX_UpdateEffectPartialTrail(
    FxSystem *system,
    FxEffect *effect,
    FxTrail *trail,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd,
    float distanceTravelledBegin,
    float distanceTravelledEnd,
    uint16_t trailElemHandleStart,
    uint16_t trailElemHandleStop,
    FxSpatialFrame *frameNow);
void FX_TrailElem_CompressBasis(const float (*inBasis)[3], char (*outBasis)[3]);
FxUpdateResult FX_UpdateTrailElement(
    FxSystem *system,
    FxEffect *effect,
    FxTrail *trail,
    FxTrailElem *trailElem,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd);
void FX_UpdateSpotLight(FxCmd *cmd);
void FX_UpdateSpotLightEffect(FxSystem* system, FxEffect* effect);
void FX_UpdateSpotLightEffectPartial(
    FxSystem* system,
    FxEffect* effect,
    int32_t msecUpdateBegin,
    int32_t msecUpdateEnd);
void FX_UpdateEffectBolt(FxSystem *system, FxEffect *effect);
void FX_UpdateNonDependent(FxCmd *cmd);
void FX_Update(FxSystem *system, int32_t localClientNum, bool nonBoltedEffectsOnly);
void FX_UpdateEffect(FxSystem* system, FxEffect* effect);
bool FX_ShouldProcessEffect(FxSystem *system, FxEffect *effect, bool nonBoltedEffectsOnly);
void FX_RunPhysics(int32_t localClientNum);
void FX_UpdateRemaining(FxCmd *cmd);
void FX_BeginUpdate(int32_t localClientNum);
void FX_EndUpdate(int32_t localClientNum);
void FX_AddNonSpriteDrawSurfs(FxCmd *cmd);
void FX_RewindTo(int32_t localClientNum, int32_t time);
void FX_SetNextUpdateCamera(int32_t localClientNum, const refdef_s *refdef, float zfar);
void FX_SetNextUpdateTime(int32_t localClientNum, int32_t time);
void FX_FillUpdateCmd(int32_t localClientNum, FxCmd *cmd);



// fx_curve
struct FxCurve // sizeof=0xC
{
    int32_t dimensionCount;
    int32_t keyCount;
    float keys[1];
};
static_assert(sizeof(FxCurve) == 0xC);

struct FxCurveIterator // sizeof=0x8
{                                       // ...
    const FxCurve *master;
    int32_t currentKeyIndex;
};
static_assert(LAYOUT_32BIT(sizeof(FxCurveIterator) == 0x8));

double FxCurve_Interpolate1d(const float *key, float intermediateTime);
void FxCurve_Interpolate3d(const float *key, float intermediateTime, float *result);
void FxCurveIterator_Create(FxCurveIterator *createe, const FxCurve *master);
void FxCurveIterator_Release(FxCurveIterator *releasee);

const FxCurve *FxCurve_AllocAndCreateWithKeys(float *keyArray, int32_t dimensionCount, int32_t keyCount);
void FxCurveIterator_SampleTimeVec3(FxCurveIterator *source, float *replyVector, float time);
double FxCurveIterator_SampleTime(FxCurveIterator *source, float time);
void FxCurveIterator_MoveToTime(FxCurveIterator *source, float time);



// fx_load_obj
struct FxEditorElemAtlas // sizeof=0x1C
{                                       // ...
    int32_t behavior;
    int32_t index;
    int32_t fps;
    int32_t loopCount;
    int32_t colIndexBits;
    int32_t rowIndexBits;
    int32_t entryCount;
};
static_assert(sizeof(FxEditorElemAtlas) == 0x1C);

struct FxEditorTrailDef // sizeof=0x608
{                                       // ...
    FxTrailVertex verts[64];
    int32_t vertCount;
    uint16_t inds[128];
    int32_t indCount;
};
static_assert(sizeof(FxEditorTrailDef) == 0x608);

struct FxEditorElemDef // sizeof=0x858
{                                       // ...
    char name[48];
    int32_t editorFlags;
    int32_t flags;
    FxFloatRange spawnRange;
    FxFloatRange fadeInRange;
    FxFloatRange fadeOutRange;
    float spawnFrustumCullRadius;
    FxSpawnDefLooping spawnLooping;
    FxSpawnDefOneShot spawnOneShot;
    FxIntRange spawnDelayMsec;
    FxIntRange lifeSpanMsec;
    FxFloatRange spawnOrigin[3];
    FxFloatRange spawnOffsetRadius;
    FxFloatRange spawnOffsetHeight;
    FxFloatRange spawnAngles[3];
    FxFloatRange angularVelocity[3];
    FxFloatRange initialRotation;
    FxFloatRange gravity;
    FxFloatRange elasticity;
    FxEditorElemAtlas atlas;
    float velScale[2][3];
    const FxCurve *velShape[2][3][2];
    float rotationScale;
    const FxCurve *rotationShape[2];
    float sizeScale[2];
    const FxCurve *sizeShape[2][2];
    float scaleScale;
    const FxCurve *scaleShape[2];
    const FxCurve *color[2];
    const FxCurve *alpha[2];
    float lightingFrac;
    float collOffset[3];
    float collRadius;
    const FxEffectDef *effectOnImpact;
    const FxEffectDef *effectOnDeath;
    int32_t sortOrder;
    const FxEffectDef *emission;
    FxFloatRange emitDist;
    FxFloatRange emitDistVariance;
    uint8_t elemType;
    // padding byte
    // padding byte
    // padding byte
    int32_t visualCount;
    //$6DCA2FC3F9FD742A3C1907AE7E70399A ___u41;
    union
    {
        FxElemVisuals visuals[32];
        FxElemMarkVisuals markVisuals[16];
    };
    int32_t trailSplitDist;
    int32_t trailRepeatDist;
    float trailScrollTime;
    FxEditorTrailDef trailDef;
};
static_assert(LAYOUT_32BIT(sizeof(FxEditorElemDef) == 0x858));

struct FxEditorEffectDef // sizeof=0x10B44
{                                       // ...
    char name[64];
    int32_t elemCount;
    FxEditorElemDef elems[32];
};
static_assert(LAYOUT_32BIT(sizeof(FxEditorEffectDef) == 0x10B44));

struct FxElemField // sizeof=0x8
{                                       // ...
    const char *keyName;                // ...
    bool(*handler)(const char **, FxEditorElemDef *); // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxElemField) == 0x8));

struct FxFlagOutputSet // sizeof=0xC
{                                       // ...
    int32_t *flags[3];                      // ...
};
static_assert(LAYOUT_32BIT(sizeof(FxFlagOutputSet) == 0xC));

struct FxFlagDef // sizeof=0x10
{
    const char *name;
    int32_t flagType;
    int32_t mask;
    int32_t value;
};
static_assert(LAYOUT_32BIT(sizeof(FxFlagDef) == 0x10));

enum FxSampleChannel : int32_t
{                                       // ...
    FX_CHAN_RGBA = 0x0,
    FX_CHAN_SIZE_0 = 0x1,
    FX_CHAN_SIZE_1 = 0x2,
    FX_CHAN_SCALE = 0x3,
    FX_CHAN_ROTATION = 0x4,
    FX_CHAN_COUNT = 0x5,
    FX_CHAN_NONE = 0x6,
};

const FxEffectDef *FX_Register(const char *name);
const FxEffectDef *FX_Register_FastFile(const char *name);
void FX_RegisterDefaultEffect();
struct PhysPreset *FX_RegisterPhysPreset(const char *name);
void FX_ForEachEffectDef(void(* callback)(const FxEffectDef*, void*), void* data);
void FX_UnregisterAll();

// fx_convert
const FxEffectDef *FX_Convert(const FxEditorEffectDef *editorEffect, void *(* Alloc)(uint32_t));
int32_t FX_DecideIntervalLimit(const FxEditorElemDef *edElemDef);

extern const float fx_randomTable[507];
extern int32_t fx_serverVisClient;
