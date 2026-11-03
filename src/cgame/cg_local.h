#pragma once

#include <bgame/bg_local.h>

#include <gfx_d3d/fxprimitives.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>

#include <cstdint>

struct cg_s;

struct Font_s;
struct Material;
struct ScreenPlacement;

#define MAX_EFFECT_NAMES 100

#define ACTIONSLOTS_NUM 3


#define MYMODELCOUNT 4

#define MAX_WEAPONS 128

#define ITEM_WEAPMODEL(x) (MAX_WEAPONS * (x / MAX_WEAPONS))

#define PRIMARY_LIGHT_NONE 0

static const float up[3] = { 0.0f, 0.0f, 1.0f };

enum $73E480FCE7B67BAA29FC24DF5A08B1FF : __int32
{
    WEAP_ANIM_VIEWMODEL_START = 0x0,
    WEAP_ANIM_VIEWMODEL_END = 0x1F,
};

enum $53B7CF4E68BA96864516EAE91DEE3467 : __int32
{
    IMPACTEFFECT_HEADSHOT = 0x1,
    IMPACTEFFECT_FATAL = 0x2,
    IMPACTEFFECT_EXIT = 0x4,
};

void CG_DrawRotatedPicPhysical(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float width,
    float height,
    float angle,
    const float *color,
    Material *material);
void CG_DrawRotatedPic(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float width,
    float height,
    int32_t horzAlign,
    int32_t vertAlign,
    float angle,
    const float *color,
    Material *material);
void CG_DrawRotatedQuadPic(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    const float (*verts)[2],
    float angle,
    const float *color,
    Material *material);
void CG_DrawVLine(
    const ScreenPlacement *scrPlace,
    float x,
    float top,
    float lineWidth,
    float height,
    int32_t horzAlign,
    int32_t vertAlign,
    const float *color,
    Material *material);
void CG_DrawStringExt(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    char *string,
    const float *setColor,
    int32_t forceColor,
    int32_t shadow,
    float charHeight);
int32_t CG_DrawDevString(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float xScale,
    float yScale,
    char *s,
    const float *color,
    char align,
    Font_s *font);
int32_t CG_DrawBigDevString(const ScreenPlacement *scrPlace, float x, float y, char *s, float alpha, char align);
int32_t CG_DrawBigDevStringColor(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    char *s,
    const float *color,
    char align);
int32_t CG_DrawSmallDevStringColor(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    char *s,
    const float *color,
    char align);
double CG_FadeAlpha(int32_t timeNow, int32_t startMsec, int32_t totalMsec, int32_t fadeMsec);
float *CG_FadeColor(int32_t timeNow, int32_t startMsec, int32_t totalMsec, int32_t fadeMsec);
void CG_MiniMapChanged(int32_t localClientNum);
void CG_NorthDirectionChanged(int32_t localClientNum);
void CG_DebugLine(const float *start, const float *end, const float *color, int32_t depthTest, int32_t duration);
void CG_DebugStar(const float *point, const float *color, int32_t duration);
void CG_DebugStarWithText(
    const float *point,
    const float *starColor,
    const float *textColor,
    char *string,
    float fontsize,
    int32_t duration);
void CG_DebugBox(
    const float *origin,
    const float *mins,
    const float *maxs,
    float yaw,
    const float *color,
    int32_t depthTest,
    int32_t duration);
void CG_DebugBoxOriented(
    const float *origin,
    const float *mins,
    const float *maxs,
    const mat3x3 &rotation,
    const float *color,
    int32_t depthTest,
    int32_t duration);
void CG_DebugCircle(
    const float *center,
    float radius,
    const float *dir,
    const float *color,
    int32_t depthTest,
    int32_t duration);
void CG_TeamColor(int32_t team, const char *prefix, float *color);
void CG_RelativeTeamColor(int32_t clientNum, const char *prefix, float *color, int32_t localClientNum);
void CG_Draw2DLine(
    const ScreenPlacement *scrPlace,
    float p1x,
    float p1y,
    float p2x,
    float p2y,
    float lineWidth,
    int horzAlign,
    int vertAlign,
    const float *color,
    Material *material);



// cg_hudelem
struct cg_hudelem_t // sizeof=0x238
{                                       // ...
    float x;                            // ...
    float y;
    float width;
    float height;
    char hudElemLabel[256];             // ...
    float labelWidth;                   // ...
    char hudElemText[256];              // ...
    float textWidth;
    Font_s *font;
    float fontScale;
    float fontHeight;
    float color[4];                     // ...
    int32_t timeNow;                        // ...
};
struct hudelem_s;
void CG_HudElemRegisterDvars();
void CG_TranslateHudElemMessage(
    int32_t localClientNum,
    const char *message,
    const char *messageType,
    char *hudElemString);
char ReplaceDirective(int32_t localClientNum, uint32_t *searchPos, uint32_t *dstLen, char *dstString);
void GetHudelemDirective(int32_t localClientNum, char *directive, char *result);
void DirectiveFakeIntroSeconds(int32_t localClientNum, const char *arg0, char *result);
void ParseDirective(char *directive, char *resultName, char *resultArg0);
void CG_Draw2dHudElems(int32_t localClientNum, int32_t foreground);
void DrawSingleHudElem2d(int32_t localClientNum, const hudelem_s *elem);
void GetHudElemInfo(int32_t localClientNum, const hudelem_s *elem, cg_hudelem_t *cghe, char *hudElemString);
void SafeTranslateHudElemString(int32_t localClientNum, int32_t index, char *hudElemString);
double HudElemStringWidth(const char *string, const cg_hudelem_t *cghe);
char *HudElemTimerString(const hudelem_s *elem, int32_t timeNow);
int32_t GetHudElemTime(const hudelem_s *elem, int32_t timeNow);
char *HudElemTenthsTimerString(const hudelem_s *elem, int32_t timeNow);
float HudElemWidth(const ScreenPlacement *scrPlace, const hudelem_s *elem, const cg_hudelem_t *cghe);
double HudElemMaterialWidth(const ScreenPlacement *scrPlace, const hudelem_s *elem, const cg_hudelem_t *cghe);
double HudElemMaterialSpecifiedWidth(
    const ScreenPlacement *scrPlace,
    char alignScreen,
    int32_t sizeVirtual,
    const cg_hudelem_t *cghe);
float HudElemHeight(const ScreenPlacement *scrPlace, const hudelem_s *elem, const cg_hudelem_t *cghe);
double HudElemMaterialHeight(const ScreenPlacement *scrPlace, const hudelem_s *elem, const cg_hudelem_t *cghe);
double HudElemMaterialSpecifiedHeight(
    const ScreenPlacement *scrPlace,
    char alignScreen,
    int32_t sizeVirtual,
    const cg_hudelem_t *cghe);
void SetHudElemPos(const ScreenPlacement *scrPlace, const hudelem_s *elem, cg_hudelem_t *cghe);
void GetHudElemOrg(
    const ScreenPlacement *scrPlace,
    int32_t alignOrg,
    int32_t alignScreen,
    float xVirtual,
    float yVirtual,
    float width,
    float height,
    float *orgX,
    float *orgY);
double AlignHudElemX(int32_t alignOrg, float x, float width);
double AlignHudElemY(int32_t alignOrg, float y, float height);
double HudElemMovementFrac(const hudelem_s *elem, int32_t timeNow);
void ConsolidateHudElemText(cg_hudelem_t *cghe, char *hudElemString);
void CopyStringToHudElemString(char *string, char *hudElemString);
void HudElemColorToVec4(const hudelem_color_t *hudElemColor, float *resultColor);
void DrawHudElemString(
    uint32_t localClientNum,
    const ScreenPlacement *scrPlace,
    char *text,
    const hudelem_s *elem,
    cg_hudelem_t *cghe);
double OffsetHudElemY(const hudelem_s *elem, const cg_hudelem_t *cghe, float offsetY);
void DrawHudElemClock(int32_t localClientNum, const hudelem_s *elem, const cg_hudelem_t *cghe);
void DrawHudElemMaterial(int32_t localClientNum, const hudelem_s *elem, cg_hudelem_t *cghe);
void DrawOffscreenViewableWaypoint(int32_t localClientNum, const hudelem_s *elem);
char WorldPosToScreenPos(int32_t localClientNum, const float *worldPos, float *outScreenPos);
bool ClampScreenPosToEdges(
    int32_t localClientNum,
    float *point,
    float padLeft,
    float padRight,
    float padTop,
    float padBottom,
    float *resultNormal,
    float *resultDist);
float GetScaleForDistance(int32_t localClientNum, const float *worldPos);
int32_t GetSortedHudElems(int32_t localClientNum, hudelem_s **elems);
void CopyInUseHudElems(hudelem_s **elems, int32_t *elemCount, hudelem_s *elemSrcArray, int32_t elemSrcArrayCount);
int32_t compare_hudelems(const void *pe0, const void *pe1);
void CG_AddDrawSurfsFor3dHudElems(int32_t localClientNum);
void  AddDrawSurfForHudElemWaypoint(int32_t localClientNum, const hudelem_s *elem);
float HudElemWaypointHeight(int32_t localClientNum, const hudelem_s *elem);


// cg_weapons
struct refdef_s;
struct weaponInfo_s // sizeof=0x44
{                                       // ...
    DObj_s *viewModelDObj;
    XModel *handModel;
    XModel *gogglesModel;
    XModel *rocketModel;
    XModel *knifeModel;
    uint8_t weapModelIdx;
    // padding byte
    // padding byte
    // padding byte
    uint32_t partBits[4];
    int32_t iPrevAnim;
#ifdef KISAK_SP
    int32_t hasAnimTree;
#endif
    XAnimTree_s *tree;
    int32_t registered;
    const gitem_s *item;
    const char *translatedDisplayName;
    const char *translatedModename;
    const char *translatedAIOverlayDescription;
};
bool CG_JavelinADS(int32_t localClientNum);
int32_t CG_WeaponDObjHandle(int32_t weaponNum);
void CG_RegisterWeapon(int32_t localClientNum, uint32_t weaponNum);
XAnimTree_s *CG_CreateWeaponViewModelXAnim(WeaponDef *weapDef);
void CG_UpdateWeaponViewmodels(int32_t localClientNum);
void ChangeViewmodelDobj(
    int32_t localClientNum,
    uint32_t weaponNum,
    uint8_t weaponModel,
    XModel *newHands,
    XModel *newGoggles,
    XModel *newRocket,
    XModel *newKnife,
    bool updateClientInfo);
void CG_UpdateHandViewmodels(int32_t localClientNum, XModel *handModel);
void CG_RegisterItemVisuals(int32_t localClientNum, uint32_t weapIdx);
void CG_RegisterItems(int32_t localClientNum);
void CG_HoldBreathInit(cg_s *cgameGlob);
void CG_UpdateViewModelPose(const DObj_s *obj, int32_t localClientNum);
#ifdef KISAK_MP
bool CG_IsPlayerCrouching(clientInfo_t *ci, const centity_s *cent);
bool CG_IsPlayerProne(clientInfo_t *ci, const centity_s *cent);
bool CG_IsPlayerADS(clientInfo_t *ci, const centity_s *cent);
#endif
void CG_GuessSpreadForWeapon(
    int32_t localClientNum,
    const centity_s *cent,
    const WeaponDef *weapDef,
    float *minSpread,
    float *maxSpread);
void CG_GetPlayerViewOrigin(int32_t localClientNum, const playerState_s *ps, float *origin);
void CG_AddPlayerWeapon(
    int32_t localClientNum,
    const GfxScaledPlacement *placement,
    const playerState_s *ps,
    centity_s *cent,
    int32_t bDrawGun);
void WeaponFlash(
    int32_t localClientNum,
    uint32_t dobjHandle,
    uint32_t weaponNum,
    int32_t bViewFlash,
    uint32_t flashTag);
void HoldBreathUpdate(int32_t localClientNum);
void HoldBreathSoundLerp(int32_t localClientNum, float lerp);
void CG_UpdateViewWeaponAnim(int32_t localClientNum);
void WeaponRunXModelAnims(int32_t localClientNum, const playerState_s *ps, weaponInfo_s *weapInfo);
void StartWeaponAnim(
    int32_t localClientNum,
    uint32_t weaponNum,
    DObj_s *obj,
    int32_t animIndex,
    float transitionTime);
double GetWeaponAnimRate(WeaponDef *weapDef, XAnim_s *anims, uint32_t animIndex);
void PlayADSAnim(float weaponPosFrac, int32_t weaponNum, DObj_s *obj, int32_t animIndex);
void ResetWeaponAnimTrees(int32_t localClientNum, const playerState_s *ps);
char UpdateViewmodelAttachments(
    int32_t localClientNum,
    uint32_t weaponNum,
    uint8_t weaponModel,
    weaponInfo_s *weapInfo);
bool ViewmodelRocketShouldBeAttached(int32_t localClientNum, WeaponDef *weapDef);
bool ViewmodelKnifeShouldBeAttached(int32_t localClientNum, WeaponDef *weapDef);

#ifdef KISAK_SP
bool CG_NVGViewModelShouldBeAttached(int32_t localClientNum);
#endif

void ProcessWeaponNoteTracks(int32_t localClientNum, const playerState_s *predictedPlayerState);
void PlayNoteMappedSoundAliases(int32_t localClientNum, const char *noteName, const WeaponDef *weapDef);
void CG_AddViewWeapon(int32_t localClientNum);
void CalculateWeaponPosition_Sway(cg_s *cgameGlob);
void CalculateWeaponPosition(cg_s *cgameGlob, float *origin);
void CalculateWeaponPosition_SwayMovement(const cg_s *cgameGlob, float *origin);
void CalculateWeaponPosition_BasePosition(cg_s *cgameGlob, float *origin);
void CalculateWeaponPosition_BasePosition_movement(cg_s *cgameGlob, float *origin);
void CalculateWeaponPosition_ToWorldPosition(const cg_s *cgameGlob, float *origin);
void CalculateWeaponPosition_SaveOffsetMovement(cg_s *cgameGlob, float *origin);
void CalculateWeaponPostion_PositionToADS(cg_s *cgameGlob, playerState_s *ps);
void CG_NextWeapon_f();
bool WeaponCycleAllowed(cg_s *cgameGlob);
void CG_PrevWeapon_f();
void CG_OutOfAmmoChange(int32_t localClientNum);
char VerifyPlayerAltModeWeapon(int32_t localClientNum, const WeaponDef *weapDef);
char CycleWeapPrimary(int32_t localClientNum, int32_t cycleForward, int32_t bIgnoreEmpty);
uint32_t CG_AltWeaponToggleIndex(int32_t localClientNum, const cg_s *cgameGlob);
int32_t NextWeapInCycle(
    int32_t localClientNum,
    const playerState_s *ps,
    weapInventoryType_t type,
    uint32_t startWeaponIndex,
    bool cycleForward,
    bool skipEmpties,
    bool skipHaveNoAlts);
void CG_ActionSlotDown_f();
char ToggleWeaponAltMode(int32_t localClientNum);
bool ActionSlotUsageAllowed(cg_s *cgameGlob);
char ActionParms(int32_t *slotResult);
void CG_ActionSlotUp_f();
void CG_EjectWeaponBrass(int32_t localClientNum, const entityState_s *ent, int32_t event);
void CG_FireWeapon(
    int32_t localClientNum,
    centity_s *cent,
    int32_t event,
    uint16_t tagName,
    uint32_t weapon,
    const playerState_s *ps);
void DrawBulletImpacts(
    int32_t localClientNum,
    const centity_s *ent,
    const WeaponDef *weaponDef,
    uint16_t boneName,
    const playerState_s *ps);
void FireBulletPenetrate(
    int32_t localClientNum,
    BulletFireParams *bp,
    const WeaponDef *weapDef,
    const centity_s *attacker,
    float *tracerStart,
    bool drawTracer);
char BulletTrace(
    int32_t localClientNum,
    const BulletFireParams *bp,
    const WeaponDef *weapDef,
    const centity_s *attacker,
    BulletTraceResults *br,
    uint32_t lastSurfaceType);
bool ShouldIgnoreHitEntity(int32_t attackerNum, int32_t hitEntNum);
bool IsEntityAPlayer(int32_t localClientNum, uint32_t entityNum);
void CG_BulletEndpos(
    int32_t randSeed,
    float spread,
    const float *start,
    float *end,
    float *dir,
    const float *forwardDir,
    const float *rightDir,
    const float *upDir,
    float maxRange);
void RandomBulletDir(int32_t randSeed, float *x, float *y);
void TakeClipOnlyWeaponIfEmpty(int32_t localClientNum, playerState_s *ps);
void CG_SpawnTracer(int32_t localClientNum, const float *pstart, const float *pend);
void CG_DrawTracer(const float *start, const float *finish, const refdef_s *refdef);
void ScaleTracer(
    const float *start,
    const float *finish,
    const float *viewOrg,
    float *startWidth,
    float *finishWidth);
double CalcTracerFinalScale(float tracerScaleDistRange, float dist, float tracerScale);
cg_s *CG_GetLocalClientGlobalsForEnt(int32_t localClientNum, int32_t entityNum);
void CG_GetViewDirection(int32_t localClientNum, int32_t entityNum, float *forward, float *right, float *up);
void CG_CalcEyePoint(int32_t localClientNum, int32_t entityNum, float *eyePos);
void CG_RandomEffectAxis(const float *forward, float *left, float *up);
void CG_ImpactEffectForWeapon(
    uint32_t weaponIndex,
    uint32_t surfType,
    char impactFlags,
    const FxEffectDef **outFx,
    snd_alias_list_t **outSnd);
void CG_BulletHitEvent(
    int32_t localClientNum,
    int32_t sourceEntityNum,
    uint32_t targetEntityNum,
    uint32_t weaponIndex,
    float *startPos,
    float *position,
    const float *normal,
    uint32_t surfType,
    int32_t event,
    uint8_t eventParam,
    int32_t damage,
    __int16 hitContents);
int32_t CalcMuzzlePoint(int32_t localClientNum, int32_t entityNum, float *muzzle, uint32_t flashTag);
void CG_BulletHitEvent_Internal(
    int32_t localClientNum,
    int32_t sourceEntityNum,
    uint32_t targetEntityNum,
    uint32_t weaponIndex,
    float *startPos,
    float *position,
    const float *normal,
    uint32_t surfType,
    int32_t event,
    uint8_t eventParam,
    int32_t damage,
    __int16 hitContents);
void BulletTrajectoryEffects(
    int32_t localClientNum,
    int32_t sourceEntityNum,
    float *startPos,
    float *position,
    int32_t surfType,
    uint32_t flashTag,
    uint8_t impactFlags,
    int32_t damage);
void WhizbySound(int32_t localClientNum, const float *start, const float *end);
bool ShouldSpawnTracer(int32_t localClientNum, int32_t sourceEntityNum);
void CG_BulletHitClientEvent(
    int32_t localClientNum,
    int32_t sourceEntityNum,
    float *startPos,
    float *position,
    uint32_t surfType,
    int32_t event,
    int32_t damage);
void CG_MeleeBloodEvent(int32_t localClientNum, const centity_s *cent);
void CG_SetupWeaponDef(int32_t localClientNum);
void ParseWeaponDefFiles(const char **ppszFiles, int32_t iNumFiles);
uint32_t ValidLatestPrimaryWeapIdx(uint32_t weaponIndex);
void CG_SelectWeaponIndex(int32_t localClientNum, uint32_t weaponIndex);
char CG_ScopeIsOverlayed(int32_t localClientNum);
int32_t CG_PlayerTurretWeaponIdx(int32_t localClientNum);
bool CG_PlayerUsingScopedTurret(int32_t localClientNum);
void CG_DisplayViewmodelAnim(int localClientNum);
char Bullet_Trace(
    const BulletFireParams *bp,
    const WeaponDef *weapDef,
    gentity_s *attacker,
    BulletTraceResults *br,
    uint32_t lastSurfaceType);

#ifdef KISAK_SP
void CG_SaveViewModelAnimTrees(struct SaveGame *save);
void CG_LoadViewModelAnimTrees(struct SaveGame *save, const struct playerState_s *ps);
void CG_ArchiveWeaponInfo(struct MemoryFile *memFile);
#endif



// cg_localents
enum leType_t : __int32
{                                       // ...
    LE_MOVING_TRACER = 0x0,
};
struct localEntity_s // sizeof=0x50
{                                       // ...
    localEntity_s *prev;
    localEntity_s *next;                // ...
    leType_t leType;
    int32_t endTime;
    trajectory_t pos;
    float color[4];
    float tracerClipDist;
    GfxEntity refEntity;
};
void TRACK_cg_localents();
void CG_InitLocalEntities(int32_t localClientNum);
localEntity_s *CG_AllocLocalEntity(int32_t localClientNum);
void CG_FreeLocalEntity(int32_t localClientNum, localEntity_s *le);
void CG_AddLocalEntityTracerBeams(int32_t localClientNum);
void CG_AddMovingTracer(const cg_s *cgameGlob, localEntity_s *le, const refdef_s *refdef);



// offhandweapons
void CG_OffhandRegisterDvars();
void CG_DrawOffHandIcon(
    int32_t localClientNum,
    const rectDef_s *rect,
    float scale,
    const float *color,
    Material *material,
    OffhandClass weaponType);
int32_t GetBestOffhand(const playerState_s *predictedPlayerState, int32_t offhandClass);
bool IsOffHandDisplayVisible(const cg_s *cgameGlob);
void CG_DrawOffHandHighlight(
    int32_t localClientNum,
    const rectDef_s *rect,
    float scale,
    const float *color,
    Material *material,
    OffhandClass weaponType);
void OffHandFlash(const cg_s *cgameGlob, const float *base_color, float *out_color);
int32_t CalcOffHandAmmo(const playerState_s *predictedPlayerState, int32_t weaponType);
void CG_DrawOffHandAmmo(
    int32_t localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    const float *color,
    int32_t textStyle,
    OffhandClass weaponType);
void CG_DrawOffHandName(
    int32_t localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    const float *color,
    int32_t textStyle,
    OffhandClass weaponType);
void CG_SwitchOffHandCmd(int32_t localClientNum);
void CG_PrepOffHand(int32_t localClientNum, const entityState_s *ent, uint32_t weaponIndex);
void CG_UseOffHand(int32_t localClientNum, const centity_s *cent, uint32_t weaponIndex);
void CG_SetEquippedOffHand(int32_t localClientNum, uint32_t offHandIndex);



// cg_world
bool CG_IsEntityLinked(int32_t localClientNum, uint32_t entIndex);
bool CG_EntityNeedsLinked(int32_t localClientNum, uint32_t entIndex);
DObj_s *CG_LocationalTraceDObj(int32_t localClientNum, uint32_t entIndex);
void CG_UnlinkEntity(int32_t localClientNum, uint32_t entIndex);
void CG_LinkEntity(int32_t localClientNum, uint32_t entIndex);
void CG_GetEntityBModelBounds(const centity_s *cent, float *mins, float *maxs, float *absMins, float *absMaxs);
void CG_GetEntityDobjBounds(const centity_s *cent, const DObj_s *dobj, float *absMins, float *absMaxs);
void CG_LocationalTrace(trace_t *results, float *start, float *end, int32_t passEntityNum, int32_t contentMask);
void CG_Trace(
    trace_t *results,
    float *start,
    float *mins,
    float *maxs,
    float *end,
    int32_t passEntityNum,
    int32_t contentMask,
    bool locational,
    bool staticModels);
void CG_ClipMoveToEntities(const moveclip_t *clip, trace_t *results);
void CG_ClipMoveToEntities_r(
    const moveclip_t *clip,
    uint16_t sectorIndex,
    const float *p1,
    const float *p2,
    trace_t *results);
void CG_ClipMoveToEntity(const moveclip_t *clip, uint32_t entIndex, trace_t *results);
int32_t CG_GetEntityBModelContents(const centity_s *cent);
void CG_PointTraceToEntities(const pointtrace_t *clip, trace_t *results);
void CG_PointTraceToEntities_r(
    const pointtrace_t *clip,
    uint16_t sectorIndex,
    const float *p1,
    const float *p2,
    trace_t *results);
void CG_PointTraceToEntity(const pointtrace_t *clip, uint32_t entIndex, trace_t *results);
void CG_LocationTraceDobjCalcPose(const DObj_s *dobj, const cpose_t *pose, int32_t *partBits);
void CG_LocationalTraceEntitiesOnly(
    trace_t *results,
    float *start,
    float *end,
    int32_t passEntityNum,
    int32_t contentMask);
void CG_TraceCapsule(
    trace_t *results,
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    int32_t passEntityNum,
    int32_t contentMask);



// cg_visionsets
enum visionSetMode_t : __int32
{                                       // ...
    VISIONSETMODE_NAKED = 0x0,
    VISIONSETMODE_NIGHT = 0x1,
    VISIONSETMODECOUNT = 0x2,
};
enum visionSetLerpStyle_t : __int32
{                                       // ...
    VISIONSETLERP_UNDEFINED = 0x0,
    VISIONSETLERP_NONE = 0x1,
    VISIONSETLERP_TO_LINEAR = 0x2,
    VISIONSETLERP_TO_SMOOTH = 0x3,
    VISIONSETLERP_BACKFORTH_LINEAR = 0x4,
    VISIONSETLERP_BACKFORTH_SMOOTH = 0x5,
};
struct visionSetLerpData_t // sizeof=0xC
{                                       // ...
    int32_t timeStart;
    int32_t timeDuration;
    visionSetLerpStyle_t style;
};
struct visionSetVars_t // sizeof=0x50
{                                       // ...
    bool glowEnable;                    // ...
    // padding byte
    // padding byte
    // padding byte
    float glowBloomCutoff;              // ...
    float glowBloomDesaturation;        // ...
    float glowBloomIntensity0;          // ...
    float glowBloomIntensity1;          // ...
    float glowRadius0;                  // ...
    float glowRadius1;                  // ...
    float glowSkyBleedIntensity0;       // ...
    float glowSkyBleedIntensity1;       // ...
    bool filmEnable;                    // ...
    // padding byte
    // padding byte
    // padding byte
    float filmBrightness;               // ...
    float filmContrast;                 // ...
    float filmDesaturation;             // ...
    bool filmInvert;                    // ...
    // padding byte
    // padding byte
    // padding byte
    float filmLightTint[3];             // ...
    float filmDarkTint[3];              // ...
};
void CG_RegisterVisionSetsDvars();
void CG_InitVisionSetsMenu();
void CG_AddVisionSetMenuItem(XAssetHeader header);
void CG_VisionSetsUpdate(int32_t localClientNum);
void UpdateVarsLerp(
    int32_t time,
    const visionSetVars_t *from,
    const visionSetVars_t *to,
    visionSetLerpData_t *lerpData,
    visionSetVars_t *result);
bool LerpBool(bool from, bool to, float fraction, visionSetLerpStyle_t style);
double LerpFloat(float from, float to, float fraction, visionSetLerpStyle_t style);
void LerpVec3(float *from, float *to, float fraction, visionSetLerpStyle_t style, float *result);
char CG_VisionSetStartLerp_To(
    int32_t localClientNum,
    visionSetMode_t mode,
    visionSetLerpStyle_t style,
    char *nameTo,
    int32_t duration);
char GetVisionSet(int32_t localClientNum, char *name, visionSetVars_t *resultSettings);
char LoadVisionFile(const char *name, visionSetVars_t *resultSettings);
char *RawBufferOpen(const char *name, const char *formatFullPath);
char LoadVisionSettingsFromBuffer(const char *buffer, const char *filename, visionSetVars_t *settings);
char ApplyTokenToField(uint32_t fieldNum, const char *token, visionSetVars_t *settings);
char VisionSetCurrent(int32_t localClientNum, visionSetMode_t mode, char *name);
void SetDefaultVision(int32_t localClientNum);
void CG_VisionSetConfigString_Naked(int32_t localClientNum);
void CG_VisionSetConfigString_Night(int32_t localClientNum);
void CG_VisionSetMyChanges();
void CG_VisionSetUpdateTweaksFromFile_Glow();
bool LoadVisionFileForTweaks(visionSetVars_t *setVars);
void CG_VisionSetUpdateTweaksFromFile_Film();
char CG_LookingThroughNightVision(int32_t localClientNum);
void CG_VisionSetApplyToRefdef(int32_t localClientNum);
double VisionFadeValue(int32_t localClientNum);
void FadeRefDef(refdef_s *rd, float brightness);



// cg_shellshock
void CG_PerturbCamera(cg_s *cgameGlob);
int32_t CG_DrawShellShockSavedScreenBlendBlurred(
    int32_t localClientNum,
    const shellshock_parms_t *parms,
    int32_t start,
    int32_t duration);
void SaveScreenToBuffer(int32_t localClientNum);
int32_t CG_DrawShellShockSavedScreenBlendFlashed(
    int32_t localClientNum,
    const shellshock_parms_t *parms,
    int32_t start,
    int32_t duration);
double BlendSmooth(float percent);
void CG_UpdateShellShock(int32_t localClientNum, const shellshock_parms_t *parms, int32_t start, int32_t duration);
void EndShellShock(int32_t localClientNum);
void EndShellShockSound(int32_t localClientNum);
void EndShellShockLookControl(int32_t localClientNum);
void EndShellShockCamera(int32_t localClientNum);
void EndShellShockScreen(int32_t localClientNum);
void UpdateShellShockSound(int32_t localClientNum, const shellshock_parms_t *parms, int32_t time, int32_t duration);
void UpdateShellShockLookControl(int32_t localClientNum, const shellshock_parms_t *parms, int32_t time, int32_t duration);
void UpdateShellShockCamera(int32_t localClientNum, const shellshock_parms_t *parms, int32_t time, int32_t duration);
double CubicInterpolate(float t, float x0, float x1, float x2, float x3);
void CG_StartShellShock(cg_s *cgameGlob, const shellshock_parms_t *parms, int32_t start, int32_t duration);
bool CG_Flashbanged(int32_t localClientNum);



// cg_pose_utils
void CG_UsedDObjCalcPose(cpose_t *pose);
void CG_CullIn(cpose_t *pose);



// cg_playerstate
#ifdef KISAK_MP
struct transPlayerState_t // sizeof=0x18
{                                       // ...
    int32_t damageEvent;
    int32_t eventSequence;
    int32_t events[4];
};
int32_t CG_TransitionPlayerState(int32_t localClientNum, playerState_s *ps, const transPlayerState_t *ops);
int32_t CG_CheckPlayerstateEvents(int32_t localClientNum, playerState_s *ps, const transPlayerState_t *ops);
#elif KISAK_SP
void CG_TransitionPlayerState(int32_t localClientNum, playerState_s *ps, const playerState_s *ops);
void CG_CheckPlayerstateEvents(int32_t localClientNum, playerState_s *ps, const playerState_s *ops);
#endif

void CG_Respawn(int32_t localClientNum);
void CG_DamageFeedback(int32_t localClientNum, int32_t yawByte, int32_t pitchByte, int32_t damage);



// cg_laser
enum LaserOwnerEnum : __int32
{                                       // ...
    LASER_OWNER_NON_PLAYER = 0x0,
    LASER_OWNER_PLAYER = 0x1,
};
void CG_Laser_Add(
    centity_s *cent,
    DObj_s *obj,
    cpose_t *pose,
    const float *viewerPos,
    LaserOwnerEnum laserOwner);
void CG_Laser_Add_Core(
    centity_s *cent,
    DObj_s *obj,
    orientation_t *orient,
    const float *viewerPos,
    LaserOwnerEnum laserOwner);



// cg_event
enum EquipmentSound_t : __int32
{                                       // ...
    EQS_WALKING = 0x0,
    EQS_RUNNING = 0x1,
    EQS_SPRINTING = 0x2,
    EQS_QWALKING = 0x3,
    EQS_QRUNNING = 0x4,
    EQS_QSPRINTING = 0x5,
};

enum InvalidCmdHintType : __int32
{                                       // ...
    INVALID_CMD_NONE = 0x0,
    INVALID_CMD_NO_AMMO_BULLETS = 0x1,
    INVALID_CMD_NO_AMMO_FRAG_GRENADE = 0x2,
    INVALID_CMD_NO_AMMO_SPECIAL_GRENADE = 0x3,
    INVALID_CMD_NO_AMMO_FLASH_GRENADE = 0x4,
    INVALID_CMD_STAND_BLOCKED = 0x5,
    INVALID_CMD_CROUCH_BLOCKED = 0x6,
    INVALID_CMD_TARGET_TOO_CLOSE = 0x7,
    INVALID_CMD_LOCKON_REQUIRED = 0x8,
    INVALID_CMD_NOT_ENOUGH_CLEARANCE = 0x9,
};

int32_t CG_GetBoneIndex(
    int32_t localClientNum,
    uint32_t dobjHandle,
    uint32_t boneName,
    uint8_t *boneIndex);
void CG_PlayBoltedEffect(
    int32_t localClientNum,
    const FxEffectDef *fxDef,
    uint32_t dobjHandle,
    uint32_t boneName);
void CG_EntityEvent(int32_t localClientNum, centity_s *cent, int32_t event);
void CG_Obituary(int32_t localClientNum, const entityState_s *ent);
void CG_ItemPickup(int32_t localClientNum, int32_t weapIndex);
void CG_EquipmentSound(int32_t localClientNum, int32_t entNum, bool isPlayerView, EquipmentSound_t type);
void CG_PlayFx(int32_t localClientNum, centity_s *cent, const float *angles);
void CG_PlayFxOnTag(int32_t localClientNum, centity_s *cent, int32_t eventParm);
void CG_SetInvalidCmdHint(cg_s *cgameGlob, InvalidCmdHintType hintType);
void CG_StopWeaponSound(
    int32_t localClientNum,
    bool isPlayerView,
    const WeaponDef *weaponDef,
    int32_t entitynum,
    weaponstate_t weaponstate);
void CG_CheckEvents(int32_t localClientNum, centity_s *cent);



// cg_draw_reticles
void CG_CalcCrosshairPosition(const cg_s *cgameGlob, float *x, float *y);
char CG_GetWeapReticleZoom(const cg_s *cgameGlob, float *zoom);
void CG_DrawNightVisionOverlay(int32_t localClientNum);
void CG_DrawCrosshair(int32_t localClientNum);
void CG_DrawAdsOverlay(
    int32_t localClientNum,
    const WeaponDef *weapDef,
    const float *color,
    const float *crosshairPos);
void CG_DrawFrameOverlay(
    float innerLeft,
    float innerRight,
    float innerTop,
    float innerBottom,
    const float *color,
    Material *material);
bool CG_UsingLowResViewPort(int32_t localClientNum);
void CG_UpdateScissorViewport(refdef_s *refdef, float *drawPos, float *drawSize);
double CG_DrawWeapReticle(int32_t localClientNum);
void CG_CalcCrosshairColor(int32_t localClientNum, float alpha, float *color);
void CG_DrawTurretCrossHair(int32_t localClientNum);
char AllowedToDrawCrosshair(int32_t localClientNum, const playerState_s *predictedPlayerState);
bool CG_IsReticleTurnedOff();
void CG_DrawAdsAimIndicator(
    int32_t localClientNum,
    const WeaponDef *weapDef,
    const float *color,
    float centerX,
    float centerY,
    float transScale);
void CG_TransitionToAds(
    const cg_s *cgameGlob,
    const WeaponDef *weapDef,
    float posLerp,
    float *transScale,
    float *transShift);
void CG_DrawReticleCenter(
    int32_t localClientNum,
    const WeaponDef *weapDef,
    const float *color,
    float centerX,
    float centerY,
    float transScale);
void CG_DrawReticleSides(
    int32_t localClientNum,
    const WeaponDef *weapDef,
    const float *baseColor,
    float centerX,
    float centerY,
    float transScale);
void CG_CalcReticleSpread(
    const cg_s *cgameGlob,
    const WeaponDef *weapDef,
    const float *drawSize,
    float transScale,
    float *spread);
void CG_CalcReticleColor(const float *baseColor, float alpha, float aimSpreadScale, float *reticleColor);
void CG_CalcReticleImageOffset(const float *drawSize, float *imageTexelOffset);


// cg_draw_indicators
struct HudGrenade // sizeof=0x10
{                                       // ...
    float origin[3];
    Material *material;                 // ...
};
void CG_DrawFlashDamage(const cg_s *cgameGlob);
void CG_DrawDamageDirectionIndicators(int32_t localClientNum);
void CG_ClearHudGrenades();
char CG_AddHudGrenade_PositionCheck(const cg_s *cgameGlob, const centity_s *grenadeEnt, WeaponDef *weapDef);
void CG_AddHudGrenade(const cg_s *cgameGlob, const centity_s *grenadeEnt);
void CG_DrawGrenadeIndicators(int32_t localClientNum);
void CG_DrawGrenadePointer(
    int32_t localClientNum,
    float centerX,
    float centerY,
    const float *grenadeOffset,
    const float *color);
void CG_DrawGrenadeIcon(
    int32_t localClientNum,
    float centerX,
    float centerY,
    const float *grenadeOffset,
    const float *color,
    Material *material);



// cg_draw_debug
struct meminfo_t;
void CG_CalculateFPS();
double CG_DrawFPS(const ScreenPlacement *scrPlace, float y, meminfo_t *meminfo);
bool CG_Flash(int32_t timeMs);
double CG_CornerDebugPrint(
    const ScreenPlacement *sP,
    float posX,
    float posY,
    float labelWidth,
    char *text,
    char *label,
    const float *color);
double CG_CornerDebugPrintCaption(
    const ScreenPlacement *sP,
    float posX,
    float posY,
    float labelWidth,
    char *text,
    const float *color);
void CG_DrawUpperRightDebugInfo(int32_t localClientNum);
#ifdef KISAK_MP
float CG_DrawSnapshot(int32_t localClientNum, float posY);
#endif
double CG_DrawStatmon(const ScreenPlacement *scrPlace, float y, meminfo_t *meminfo);
void CG_DrawPerformanceWarnings();
void CG_DrawDebugOverlays(int32_t localClientNum);
void CG_DrawMaterial(int32_t localClientNum, uint32_t drawMaterialType);
void CG_DrawDebugPlayerHealth(int32_t localClientNum);
void CG_DrawFullScreenDebugOverlays(int32_t localClientNum);
void CG_DrawScriptUsage(const ScreenPlacement *scrPlace);
void CG_DrawVersion();
void CG_DrawSoundEqOverlay(int32_t localClientNum);
void CG_DrawSoundOverlay(const ScreenPlacement *scrPlace);
void CG_DrawFxProfile(int32_t localClientNum);
void CG_DrawFxText(char *text, float *profilePos);
void CG_DrawFxMarkProfile(int32_t localClientNum);



// cg_compass
enum CompassType : __int32
{                                       // ...
    COMPASS_TYPE_PARTIAL = 0x0,
    COMPASS_TYPE_FULL = 0x1,
};

void CG_CompassRegisterDvars();
bool CG_IsSelectingLocation(int32_t localClientNum);
bool CG_WorldPosToCompass(
    CompassType compassType,
    const cg_s *cgameGlob,
    const rectDef_s *mapRect,
    const float *north,
    const float *playerWorldPos,
    const float *in,
    float *out,
    float *outClipped);
void CG_CompassCalcDimensions(
    CompassType compassType,
    const cg_s *cgameGlob,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    float *x,
    float *y,
    float *w,
    float *h);
double CG_FadeCompass(int32_t localClientNum, int32_t displayStartTime, CompassType compassType);
void CG_CompassDrawPlayerBack(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    float *color);
void CG_CompassDrawPlayerNorthCoord(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Font_s *font,
    Material *material,
    float *const color,
    int32_t style);
void CG_CompassDrawPlayerEastCoord(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Font_s *font,
    Material *material,
    float *const color,
    int32_t style);
void CG_CompassDrawPlayerNCoordScroll(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Font_s *font,
    Material *material,
    float *color,
    int32_t textStyle);
void CG_CompassDrawPlayerECoordScroll(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Font_s *font,
    Material *material,
    float *color,
    int32_t textStyle);
void CG_CompassDrawPlayerMap(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    float *color);
void CG_CompassDrawPlayerMapLocationSelector(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    float *color);
void CG_CompassDrawPlayer(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    rectDef_s *rect,
    Material *material,
    float *color);
void CG_CompassDrawBorder(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    rectDef_s *rect,
    Material *material,
    float *color);
void CG_CompassUpYawVector(const cg_s *cgameGlob, float *result);
void CG_CompassDrawTickertape(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    const float *color,
    Font_s *textFont,
    float textScale,
    int32_t textStyle,
    bool drawObjectives);
void CalcCompassPointerSize(CompassType compassType, float *w, float *h);
void DrawIconDistanceText(
    int32_t localClientNum,
    float distance,
    float iconX,
    float iconY,
    float iconH,
    const rectDef_s *rect,
    const float *color,
    Font_s *textFont,
    float textScale,
    int32_t textStyle);
double CutFloat(float original);
double CG_GetHudAlphaCompass(int32_t localClientNum);
void CalcCompassFriendlySize(CompassType compassType, float *w, float *h);
void CG_CompassDrawPlayerPointers_MP(
    int32_t localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    const float *color);
void CG_CompassDrawPlayerPointers_SP(
    int localClientNum,
    CompassType compassType,
    const rectDef_s *parentRect,
    const rectDef_s *rect,
    Material *material,
    float *color);
double GetObjectiveFade(const rectDef_s *clipRect, float x, float y, float width, float height);
void CG_CompassDrawGoalDistance(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    double scale,
    float *color,
    int textStyle);



// cg_colltree
struct CgEntCollNode // sizeof=0x14
{                                       // ...
    uint16_t sector;
    uint16_t nextEntInSector;
    float linkMins[2];
    float linkMaxs[2];
};
union CgEntCollTree_u // sizeof=0x2
{                                       // ...
    uint16_t parent;
    uint16_t nextFree;
};
struct CgEntCollTree // sizeof=0xC
{                                       // ...
    float dist;
    uint16_t axis;
    CgEntCollTree_u u;
    uint16_t child[2];
};
struct CgEntCollSector // sizeof=0x10
{                                       // ...
    CgEntCollTree tree;
    uint16_t entListHead;
    // padding byte
    // padding byte
};
struct CgEntCollWorld // sizeof=0x401C
{                                       // ...
    float mins[3];
    float maxs[3];
    uint16_t freeHead;
    // padding byte
    // padding byte
    CgEntCollSector sectors[1024];
};
void TRACK_CG_CollWorld();
void CG_SetCollWorldLocalClientNum(int32_t localClientNum);
int32_t CG_GetCollWorldLocalClientNum();
void CG_ClearEntityCollWorld(int32_t localClientNum);
const CgEntCollSector *CG_GetEntityCollSector(int32_t localClientNum, uint16_t sectorIndex);
const CgEntCollNode *CG_GetEntityCollNode(int32_t localClientNum, uint32_t entIndex);
CgEntCollNode *CG_GetCollNode(int32_t localClientNum, uint32_t entIndex);
void CG_UnlinkEntityColl(int32_t localClientNum, uint32_t entIndex);
void CG_LinkEntityColl(int32_t localClientNum, uint32_t entIndex, const float *absMins, const float *absMaxs);
void CG_AddEntityToCollSector(int32_t localClientNum, uint32_t entIndex, uint16_t sectorIndex);
void CG_SortEntityCollSector(
    int32_t localClientNum,
    uint16_t sectorIndex,
    const float *mins,
    const float *maxs);
uint16_t CG_AllocEntityCollSector(int32_t localClientNum, const float *mins, const float *maxs);



// cg_camerashake
struct CameraShake // sizeof=0x24
{                                       // ...
    int32_t time;                           // ...
    float scale;                        // ...
    float length;                       // ...
    float radius;                       // ...
    float src[3];                       // ...
    float size;                         // ...
    float rumbleScale;                  // ...
};
struct CameraShakeSet // sizeof=0x94
{                                       // ...
    CameraShake shakes[4];
    float phase;
};

void TRACK_cg_camerashake();
void CG_StartShakeCamera(int32_t localClientNum, float p, int32_t duration, float *src, float radius);
int32_t CG_UpdateCameraShake(const cg_s *cgameGlob, CameraShake *shake);
void CG_ShakeCamera(int32_t localClientNum);
void CG_ClearCameraShakes(int32_t localClientNum);
#ifdef KISAK_SP
void CG_ArchiveCameraShake(int localClientNum, struct MemoryFile *memFile);
#endif



// cg_ammocounter
void CG_AmmoCounterRegisterDvars();
void CG_DrawPlayerWeaponAmmoStock(
    int32_t localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    float *color,
    Material *material,
    int32_t textStyle);
uint32_t ClipCounterWeapIdx(const cg_s *cgameGlob, uint32_t weapIndex);
uint32_t GetWeaponAltIndex(const cg_s *cgameGlob, const WeaponDef *weapDef);
double AmmoCounterFadeAlpha(int32_t localClientNum, cg_s *cgameGlob);
double CG_GetHudAlphaDPad(int32_t localClientNum);
double DpadFadeAlpha(int32_t localClientNum, cg_s *cgameGlob);
bool ActionSlotIsActive(int32_t localClientNum, uint32_t slotIdx);
double CG_GetHudAlphaAmmoCounter(int32_t localClientNum);
bool CG_ActionSlotIsUsable(int32_t localClientNum, uint32_t slotIdx);
void CG_DrawPlayerActionSlotDpad(
    int32_t localClientNum,
    const rectDef_s *rect,
    const float *color,
    Material *material);
void CG_DrawPlayerActionSlot(
    int32_t localClientNum,
    const rectDef_s *rect,
    uint32_t slotIdx,
    float *color,
    Font_s *textFont,
    float textScale,
    int32_t textStyle);
void DpadIconDims(
    const rectDef_s *rect,
    uint32_t slotIdx,
    WeaponDef *weapDef,
    float *x,
    float *y,
    float *w,
    float *h);
void DpadTextPos(const rectDef_s *rect, uint32_t slotIdx, WeaponDef *weapDef, float *x, float *y);
void CG_DrawPlayerWeaponBackground(
    int32_t localClientNum,
    const rectDef_s *rect,
    const float *color,
    Material *material);
void CG_DrawPlayerWeaponAmmoClipGraphic(int32_t localClientNum, const rectDef_s *rect, const float *color);
void GetBaseRectPos(int32_t localClientNum, const rectDef_s *rect, float *base);
void DrawClipAmmo(cg_s *cgameGlob, float *base, uint32_t weapIdx, const WeaponDef *weapDef, float *color);
void DrawClipAmmoMagazine(
    cg_s *cgameGlob,
    const float *base,
    uint32_t weapIdx,
    const WeaponDef *weapDef,
    float *color);
void AmmoColor(cg_s *cgameGlob, float *color, uint32_t weapIndex);
void DrawClipAmmoShortMagazine(
    cg_s *cgameGlob,
    const float *base,
    uint32_t weapIdx,
    const WeaponDef *weapDef,
    float *color);
void DrawClipAmmoShotgunShells(
    cg_s *cgameGlob,
    const float *base,
    uint32_t weapIdx,
    const WeaponDef *weapDef,
    float *color);
void DrawClipAmmoRockets(
    cg_s *cgameGlob,
    const float *base,
    uint32_t weapIdx,
    const WeaponDef *weapDef,
    float *color);
void DrawClipAmmoBeltfed(
    cg_s *cgameGlob,
    float *base,
    uint32_t weapIdx,
    const WeaponDef *weapDef,
    float *color);
void CG_DrawPlayerWeaponIcon(int32_t localClientNum, const rectDef_s *rect, const float *color);
void DrawStretchPicGun(
    const ScreenPlacement *scrPlace,
    const rectDef_s *rect,
    const float *color,
    Material *material,
    weaponIconRatioType_t ratio);
void CG_DrawPlayerWeaponLowAmmoWarning(
    int32_t localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float textScale,
    int32_t textStyle,
    float text_x,
    float text_y,
    char textAlignMode,
    Material *material);
uint32_t GetWeaponIndex(const cg_s *cgameGlob);
int BG_PlayerHasWeapon(const playerState_s *ps, int weaponIndex);
void Vec4Copy(const float *from, float *to);


// cg_effects_load_obj
struct EffectFile // sizeof=0x630
{                                       // ...
    const char *nonflesh[12][29];       // ...
    const char *flesh[12][4];           // ...
};
FxImpactTable *CG_RegisterImpactEffects(const char *mapname);
FxImpactTable *CG_RegisterImpactEffects_LoadObj(const char *mapname);



// cg_info
void CG_LoadingString(int32_t localClientNum, const char *s);
void CG_DrawInformation(int32_t localClientNum);
bool CG_IsShowingProgress_FastFile();



extern const dvar_t *nightVisionFadeInOutTime;
extern const dvar_t *nightVisionPowerOnTime;
extern const dvar_t *nightVisionDisableEffects;

extern const dvar_t *waypointOffscreenScaleSmallest;
extern const dvar_t *waypointPlayerOffsetStand;
extern const dvar_t *waypointTweakY;
extern const dvar_t *waypointOffscreenPointerHeight;
extern const dvar_t *waypointOffscreenPadTop;
extern const dvar_t *waypointIconHeight;
extern const dvar_t *waypointDistScaleRangeMin;
extern const dvar_t *waypointOffscreenRoundedCorners;
extern const dvar_t *waypointOffscreenPadBottom;
extern const dvar_t *waypointPlayerOffsetProne;
extern const dvar_t *waypointOffscreenPointerWidth;
extern const dvar_t *waypointIconWidth;
extern const dvar_t *waypointSplitscreenScale;
extern const dvar_t *waypointPlayerOffsetCrouch;
extern const dvar_t *waypointOffscreenPadRight;
extern const dvar_t *waypointOffscreenCornerRadius;
extern const dvar_t *waypointDebugDraw;
extern const dvar_t *waypointDistScaleRangeMax;
extern const dvar_t *waypointOffscreenScaleLength;
extern const dvar_t *waypointDistScaleSmallest;
extern const dvar_t *waypointOffscreenPadLeft;
extern const dvar_t *waypointOffscreenPointerDistance;
extern const dvar_t *waypointOffscreenDistanceThresholdAlpha;
extern const dvar_t *hudElemPausedBrightness;
