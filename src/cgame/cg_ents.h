#pragma once

#if !defined(KISAK_SP) && !defined(KISAK_RADIANT)
#error This file is for SinglePlayer and Radiant only
#endif


struct DObjAnimMat;
struct ComPrimaryLight;
struct centity_s;
struct DObj_s;
struct WeaponDef;
struct cg_s;
struct entityState_s;
struct FxEffect;
struct GfxLight;
struct SaveGame;
struct cpose_t;
struct GfxSceneEntity;

int CompressUnit(double unit);
void LocalConvertQuatToMat(const DObjAnimMat *mat, float (*axis)[3]);
const ComPrimaryLight *Com_GetPrimaryLight(unsigned int primaryLightIndex);
void CG_LockLightingOrigin(centity_s *cent, float *lightingOrigin);
int CG_GetRenderFlagForRefEntity(__int16 eFlags);
void CG_General(int localClientNum, centity_s *cent);
void CG_Item(centity_s *cent);
void CG_AddEntityLoopSound(int localClientNum, const centity_s *cent);
void CG_EntityEffects(int localClientNum, centity_s *cent);
void CG_mg42_PreControllers(int localClientNum, const DObj_s *obj, centity_s *cent);
void CG_mg42(int localClientNum, centity_s *cent);
bool JavelinSoftLaunch(WeaponDef *weapDef, cg_s *cgameGlob, entityState_s *s1);
void CG_Missile(int localClientNum, centity_s *cent);
void CG_UpdateBModelWorldBounds(unsigned int localClientNum, centity_s *cent, int forceFilter);
void CG_ScriptMover(int localClientNum, centity_s *cent);
void CG_AdjustPositionForMover(
    int localClientNum,
    float *in,
    int moverNum,
    int fromTime,
    int toTime,
    float *out,
    float *outDeltaAngles);
void CG_SetFrameInterpolation(int localClientNum);
void CG_DObjUpdateInfo(const cg_s *cgameGlob, DObj_s *obj, bool notify);
struct cpose_t *CG_GetPose(int localClientNum, int handle);
void CG_Vehicle_PreControllers(int localClientNum, const DObj_s *obj, centity_s *cent);
void CG_Vehicle(int localClientNum, centity_s *cent);
void CG_SoundBlend(int localClientNum, centity_s *cent);
FxEffect *CG_StartFx(int localClientNum, centity_s *cent, int startAtTime);
void CG_Fx(int localClientNum, centity_s *cent);
void CG_LoopFx(int localClientNum, centity_s *cent);
void CG_ClampPrimaryLightOrigin(GfxLight *light, const ComPrimaryLight *refLight);
void CG_ClampPrimaryLightDir(GfxLight *light, const ComPrimaryLight *refLight);
// local variable allocation has failed, the output may be wrong!
void CG_PrimaryLight(int localClientNum, centity_s *cent);
void CG_InterpolateEntityOrigin(const cg_s *cgameGlob, centity_s *cent);
void CG_InterpolateEntityAngles(const cg_s *cgameGlob, centity_s *cent);
void CG_CreatePhysicsObject(int localClientNum, centity_s *cent);
void CG_UpdatePhysicsPose(centity_s *cent);
int CG_ExpiredLaunch(int localClientNum, centity_s *cent);
void CG_CalcEntityPhysicsPositions(int localClientNum, centity_s *cent);
void CG_SaveEntityPhysics(centity_s *cent, SaveGame *save);
void CG_LoadEntityPhysics(centity_s *cent, SaveGame *save);
void CG_CreateRagdollObject(int localClientNum, centity_s *cent);
void CG_UpdateRagdollPose(centity_s *cent);
void CG_CalcEntityRagdollPositions(int localClientNum, centity_s *cent);
void CG_CalcEntityLerpPositions(int localClientNum, centity_s *cent);
void CG_DObjCalcBone(const cpose_t *pose, DObj_s *obj, int boneIndex);
void CG_DrawEntEqDebug(const centity_s *cent);
void CG_ClearUnion(int localClientNum, centity_s *cent);
void CG_SetUnionType(int localClientNum, centity_s *cent);
void CG_UpdatePoseUnion(int localClientNum, centity_s *cent);
void CG_ProcessEntity(int localClientNum, centity_s *cent);
void CG_SaveEntityFX(centity_s *cent, SaveGame *save);
void CG_LoadEntityFX(centity_s *cent, SaveGame *save);
void CG_SaveEntity(unsigned int entnum, SaveGame *save);
void CG_LoadEntity(unsigned int entnum, SaveGame *save);
void CG_SaveEntities(SaveGame *save);
void CG_LoadEntities(SaveGame *save);
void CG_GetPoseOrigin(const cpose_t *pose, float *origin);
void CG_GetPoseAngles(const cpose_t *pose, float *angles);
float *CG_GetEntityOrigin(int localClientNum, int entnum);
void CG_GetPoseLightingHandle(const cpose_t *pose);
void CG_PredictiveSkinCEntity(GfxSceneEntity *sceneEnt);
void CG_AddPacketEntity(unsigned int localClientNum, unsigned int entnum);
int CG_AddPacketEntities(int localClientNum);
DObjAnimMat *CG_DObjGetLocalBoneMatrix(const cpose_t *pose, DObj_s *obj, int boneIndex);
DObjAnimMat *CG_DObjGetLocalTagMatrix(const cpose_t *pose, DObj_s *obj, unsigned int tagName);
int32_t CG_DObjGetWorldBoneMatrix(
    const cpose_t *pose,
    DObj_s *obj,
    int boneIndex,
    float (*tagMat)[3],
    float *origin);
int32_t CG_DObjGetWorldTagMatrix(
    const cpose_t *pose,
    DObj_s *obj,
    unsigned int tagName,
    float (*tagMat)[3],
    float *origin);
int CG_DObjGetWorldTagPos(const cpose_t *pose, DObj_s *obj, unsigned int tagName, float *pos);
