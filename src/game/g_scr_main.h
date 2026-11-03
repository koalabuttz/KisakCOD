#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <bgame/bg_public.h>

#include <bgame/bg_actor_prone.h>
#include <script/scr_variable.h>

#include <bgame/bg_local.h>
#include "actor_animapi.h"
#include "actor.h"
#include "g_public.h"

struct scr_data_t_tag
{
    scr_animtree_t tree;
};

struct BuiltinFunctionDef
{
    const char *actionString;
    void(*actionFunc)();
    int type;
};

struct corpseInfo_t
{
    XAnimTree_s *tree;
    int entnum;
    actor_prone_info_s proneInfo;
};

struct actorBackup_s
{
    ai_animmode_t eAnimMode;
    ai_animmode_t eDesiredAnimMode;
    ai_animmode_t eScriptSetAnimMode;
    bool bUseGoalWeight;
    actor_physics_t Physics;
    ai_orient_t ScriptOrient;
    ai_orient_t CodeOrient;
    float fDesiredBodyYaw;
    float currentOrigin[3];
    float currentAngles[3];
    float vLookForward[3];
    float vLookRight[3];
    float vLookUp[3];
};

struct scr_data_t
{
    int levelscript;
    int scripted_init;
    scr_data_t_tag generic_human;
    AnimScriptList anim;
    AnimScriptList dogAnim;
    int delete_;
    int initstructs;
    int createstruct;
    XAnimTree_s *actorXAnimTrees[32];
    corpseInfo_t actorCorpseInfo[16];
    XAnimTree_s *actorBackupXAnimTree;
    actorBackup_s *actorBackup;
    XAnimTree_s *actorXAnimClientTrees[64];
    bool actorXAnimClientTreesInuse[64];
    int actorFreeClientTree;
};

struct ScriptFunctions
{
    int maxSize;
    int count;
    int *address;
};

struct cached_tag_mat_t
{
    int time;
    int entnum;
    unsigned __int16 name;
    float tagMat[4][3];
};

unsigned int GScr_AllocString(const char *s);
void TRACK_g_scr_main();
void Scr_LoadLevel();
int GScr_SetScriptAndLabel(
    ScriptFunctions *functions,
    const char *filename,
    const char *label,
    int bEnforceExists);
void GScr_SetLevelScript(ScriptFunctions *functions);
void *GScr_AnimscriptAlloc(int size);
void GScr_SetScriptsForPathNode(pathnode_t *loadNode, void *data);
void GScr_SetScriptsForPathNodes(ScriptFunctions *functions);
scr_animtree_t GScr_FindAnimTree(const char *filename, int bEnforceExists);
void GScr_FindAnimTrees();
void GScr_SetSingleAnimScript(ScriptFunctions *functions, scr_animscript_t *pAnim, const char *name);
void GScr_SetAnimScripts(ScriptFunctions *functions);
void GScr_SetDogAnimScripts(ScriptFunctions *functions);
void GScr_PostLoadScripts();
void *Hunk_AllocXAnimCreate(int size);
void GScr_FreeScripts();
gentity_s *GetEntity(scr_entref_t entref);
gentity_s *GetPlayerEntity(scr_entref_t entref);
void GScr_CreatePrintChannel();
void GScr_printChannelSet();
void print();
void println();
// attributes: thunk
void Scr_LocalizationError(unsigned int iParm, const char *pszErrorMessage);
void Scr_ValidateLocalizedStringRef(unsigned int parmIndex, const char *token, int tokenLen);
int Scr_ValidateNonLocalizedStringRef(
    unsigned int parmIndex,
    const char *token,
    int tokenLen,
    const char *errorContext);
unsigned int Scr_NonLocalizedStringErrorPrefix(
    unsigned int parmIndex,
    unsigned int tokenLen,
    const char *errorContext,
    unsigned int stringLen,
    unsigned int stringLimit,
    char *string);
int Scr_IsValidMessageChar(char key);
void Scr_ConstructMessageString(
    int firstParmIndex,
    int lastParmIndex,
    const char *errorContext,
    char *string,
    unsigned int stringLimit);
void Scr_MakeGameMessage(const char *cmd);
void Scr_VerifyWeaponIndex(int weaponIndex, const char *weaponName);
void iprintln();
void iprintlnbold();
void GScr_print3d();
void GScr_line();
void assertCmd();
void assertexCmd();
void assertmsgCmd();
void GScr_IsDefined();
void GScr_IsString();
void GScr_IsArray();
void GScr_IsAlive();
void GScr_IsPlayer();
void GScr_IsAI();
void GScr_IsSentient();
void GScr_IsGodMode();
void GScr_GetDvar();
void GScr_GetDvarInt();
void GScr_GetDvarFloat();
void GScr_GetDebugDvar();
void GScr_GetDebugDvarInt();
void GScr_GetDebugDvarFloat();
void GScr_SetDvar();
void GScr_SetSavedDvar();
void GScr_GetTime();
void GScr_GetDifficulty();
void Scr_GetEntByNum();
int Scr_GetTeamFlag(const char *pszTeamName, const char *pszCaller);
int Scr_GetTeamFlags(unsigned int i, const char *pszCaller);
int Scr_GetSpecies(unsigned __int16 speciesString);
actor_s *Scr_GetAIArray();
actor_s *Scr_GetAISpeciesArray();
void Scr_GetSpawnerArray();
void Scr_GetSpawnerTeamArray();
void Scr_GetWeaponModel();
void Scr_GetWeaponClipModel();
void GScr_GetAnimLength();
void GScr_AnimHasNotetrack();
void GScr_GetNotetrackTimes();
void GScr_GetBrushModelCenter();
void GScr_GetKeyBinding();
void GScr_GetCommandFromKey();
void GScr_Spawn();
void GScr_SpawnVehicle();
void GScr_SpawnTurret();
void GScr_CanSpawnTurret();
int GScr_PrecacheTurret();
void ScrCmd_startIgnoringSpotLight(scr_entref_t entref);
void ScrCmd_stopIgnoringSpotLight(scr_entref_t entref);
void ScrCmd_attach(scr_entref_t entref);
void ScrCmd_detach(scr_entref_t entref);
void ScrCmd_detachAll(scr_entref_t entref);
void ScrCmd_GetAttachSize(scr_entref_t entref);
void ScrCmd_GetAttachModelName(scr_entref_t entref);
void ScrCmd_GetAttachTagName(scr_entref_t entref);
void ScrCmd_GetAttachIgnoreCollision(scr_entref_t entref);
void ScrCmd_hidepart(scr_entref_t entref);
void ScrCmd_showpart(scr_entref_t entref);
void ScrCmd_showallparts(scr_entref_t entref);
void ScrCmd_LinkTo(scr_entref_t entref);
void ScrCmd_SetMoveSpeedScale(scr_entref_t entref);
void ScrCmd_GetTimeScale();
void ScrCmd_SetTimeScale();
void ScrCmd_SetPhysicsGravityDir();
void ScrCmd_PlayerSetGroundReferenceEnt(scr_entref_t entref);
void ScrCmd_PlayerLinkTo(scr_entref_t entref);
void ScrCmd_PlayerLinkToDelta(scr_entref_t entref);
void ScrCmd_PlayerLinkToAbsolute(scr_entref_t entref);
void ScrCmd_Unlink(scr_entref_t entref);
void ScrCmd_EnableLinkTo(scr_entref_t entref);
void ScrCmd_DontInterpolate(scr_entref_t entref);
void ScrCmd_dospawn(scr_entref_t entref);
void ScrCmd_StalingradSpawn(scr_entref_t entref);
void ScrCmd_GetOrigin(scr_entref_t entref);
void ScrCmd_GetCentroid(scr_entref_t entref);
void ScrCmd_GetStance(scr_entref_t entref);
void ScrCmd_SetStance(scr_entref_t entref);
void ScrCmd_ItemWeaponSetAmmo(scr_entref_t entref);
void ScrCmd_MagicGrenade(scr_entref_t entref);
void ScrCmd_MagicGrenadeManual(scr_entref_t entref);
void Scr_BulletSpread();
void Scr_BulletTracer();
void Scr_MagicBullet();
void GScr_IsFiringTurret(scr_entref_t entref);
void GScr_SetFriendlyChain(scr_entref_t entref);
int GScr_UpdateTagInternal(
    gentity_s *ent,
    unsigned int tagName,
    cached_tag_mat_t *cachedTag,
    int showScriptError);
void GScr_GetTagOrigin(scr_entref_t entref);
void GScr_GetTagAngles(scr_entref_t entref);
void ScrCmd_GetEye(scr_entref_t entref);
void ScrCmd_GetDebugEye(scr_entref_t entref);
void ScrCmd_UseBy(scr_entref_t entref);
void ScrCmd_IsTouching(scr_entref_t entref);
void ParsePlaySoundCmd(scr_entref_t entref, int event, int notifyevent);
void ScrCmd_SoundExists();
void ScrCmd_PlaySound(scr_entref_t entref);
void ScrCmd_PlaySoundAsMaster(scr_entref_t entref);
void ScrCmd_PlayLoopSound(scr_entref_t entref);
void ScrCmd_StopLoopSound(scr_entref_t entref);
void ScrCmd_StopSounds(scr_entref_t entref);
void ScrCmd_EqOn(scr_entref_t entref);
void ScrCmd_EqOff(scr_entref_t entref);
void ScrCmd_HasEq(scr_entref_t entref);
void ScrCmd_IsWaitingOnSound(scr_entref_t entref);
void ScrCmd_Delete(scr_entref_t entref);
void ScrCmd_SetModel(scr_entref_t entref);
void ScrCmd_SetShadowHint(scr_entref_t entref);
// local variable allocation has failed, the output may be wrong!
void ScrCmd_GetNormalHealth(scr_entref_t entref);
void ScrCmd_SetNormalHealth(scr_entref_t entref);
void ScrCmd_DoDamage(scr_entref_t entref);
void ScrCmd_Show(scr_entref_t entref);
void ScrCmd_Hide(scr_entref_t entref);
void ScrCmd_LaserOn(scr_entref_t entref);
void ScrCmd_LaserOff(scr_entref_t entref);
void ScrCmd_SetContents(scr_entref_t entref);
void Scr_SetStableMissile(scr_entref_t entref);
void GScr_DisconnectPaths(scr_entref_t entref);
void GScr_ConnectPaths(scr_entref_t entref);
void GScr_StartFiring(scr_entref_t entref);
void GScr_StopFiring(scr_entref_t entref);
void GScr_ShootTurret(scr_entref_t entref);
void GScr_SetMode(scr_entref_t entref);
void GScr_GetTurretOwner(scr_entref_t entref);
void GScr_SetTargetEntity(scr_entref_t entref);
void GScr_SetAiSpread(scr_entref_t entref);
void GScr_SetPlayerSpread(scr_entref_t entref);
void GScr_SetConvergenceTime(scr_entref_t entref);
void GScr_SetSuppressionTime(scr_entref_t entref);
void GScr_ClearTargetEntity(scr_entref_t entref);
void GScr_SetTurretTeam(scr_entref_t entref);
void GScr_SetTurretIgnoreGoals(scr_entref_t entref);
void GScr_MakeTurretUsable(scr_entref_t entref);
void GScr_MakeTurretUnusable(scr_entref_t entref);
void GScr_SetTurretAccuracy(scr_entref_t entref);
void GScr_GetTurretTarget(scr_entref_t entref);
void GScr_SetCursorHint(scr_entref_t entref);
int G_GetHintStringIndex(int *piIndex, const char *pszString);
void GScr_SetHintString(scr_entref_t entref);
void GScr_UseTriggerRequireLookAt(scr_entref_t entref);
void G_InitObjectives();
int PrintObjectiveUpdate(unsigned int state, const char *objectiveDesc);
int ObjectiveStateIndexFromString(int *piStateIndex, unsigned int stateString);
void SetObjectiveIconIntoConfigString(char *objConfigString, unsigned int paramNum);
int Scr_Objective_Add();
void Scr_Objective_Delete();
int Scr_Objective_State();
void Scr_Objective_String_Internal(int makeUpdateMessage);
void Scr_Objective_String();
void Scr_Objective_String_NoMessage();
void Scr_Objective_Icon();
void Scr_Objective_Position();
void Scr_Objective_AdditionalPosition();
int Scr_Objective_Current();
int Scr_Objective_AdditionalCurrent();
void Scr_Objective_Ring();
void Scr_BulletTrace();
void Scr_BulletTracePassed();
void Scr_SightTracePassed();
void Scr_PhysicsTrace();
void Scr_PlayerPhysicsTrace();
void Scr_RandomInt();
void Scr_RandomFloat();
void Scr_RandomIntRange();
void Scr_RandomFloatRange();
void GScr_sin();
void GScr_cos();
void GScr_tan();
void GScr_asin();
void GScr_acos();
void GScr_atan();
void GScr_CastInt();
void GScr_abs();
void GScr_min();
void GScr_max();
void GScr_floor();
void GScr_ceil();
void GScr_sqrt();
void GScr_VectorFromLineToPoint();
void GScr_PointOnSegmentNearestToPoint();
void Scr_Distance();
void Scr_Distance2D();
void Scr_DistanceSquared();
void Scr_Length();
void Scr_LengthSquared();
void Scr_Closer();
void Scr_VectorDot();
void Scr_VectorNormalize();
void Scr_VectorToAngles();
void Scr_VectorLerp();
void Scr_AnglesToUp();
void Scr_AnglesToRight();
void Scr_AnglesToForward();
void Scr_CombineAngles();
void Scr_IsSubStr();
void Scr_GetSubStr();
void Scr_ToLower();
void Scr_StrTok();
void Scr_SetBlur();
void Scr_MusicPlay();
void Scr_MusicStop();
void Scr_SoundFade();
void Scr_Amplify();
void Scr_AmplifyStop();
void Scr_ErrorOnDefaultAsset(XAssetHeader *type, const char *assetName);
int Scr_PrecacheModel();
void Scr_PrecacheShellShock();
void Scr_PrecacheItem();
int Scr_PrecacheMaterial();
void Scr_PrecacheString();
int Scr_PrecacheRumble();
void GScr_PrecacheLocationSelector();
void Scr_PrecacheNightvisionCodeAssets();
int GScr_GetLocSelIndex(const char *mtlName);
void Scr_AmbientPlay();
void Scr_AmbientStop();
void Scr_CheckForSaveErrors(int saveId);
void Scr_SaveGame();
void Scr_SaveGameNoCommit();
void Scr_IsSaveSuccessful();
void Scr_IsRecentlyLoaded();
void Scr_CommitSave();
void GScr_RadiusDamageInternal(gentity_s *inflictor);
void GScr_RadiusDamage();
void GScr_EntityRadiusDamage(scr_entref_t entref);
void GScr_Detonate(scr_entref_t entref);
int GScr_SetPlayerIgnoreRadiusDamage();
void GScr_DamageConeTraceInternal(scr_entref_t entref, int contentMask);
void GScr_DamageConeTrace(scr_entref_t entref);
void GScr_SightConeTrace(scr_entref_t entref);
void GScr_ChangeLevel();
void GScr_MissionSuccess();
void GScr_MissionFailed();
void GScr_SetMissionDvar();
void GScr_Cinematic();
void GScr_CinematicInGame();
void GScr_CinematicInGameSync();
void GScr_CinematicInGameLoop();
void GScr_CinematicInGameLoopResident();
void GScr_CinematicInGameLoopFromFastfile();
void GScr_StopCinematicInGame();
void GScr_IsCinematicPlaying();
void GScr_Earthquake();
int GScr_DrawCompassFriendlies();
void GScr_WeaponFireTime();
void GScr_WeaponClipSize();
void GScr_GetAmmoCount(scr_entref_t entref);
void GScr_WeaponIsSemiAuto();
void GScr_WeaponIsBoltAction();
void GScr_WeaponType();
void GScr_WeaponClass();
void GScr_WeaponInventoryType();
void GScr_WeaponStartAmmo();
void GScr_WeaponMaxAmmo();
void GScr_WeaponAltWeaponName();
WeaponDef *GScr_GetWeaponDef();
void GScr_WeaponFightDist();
void GScr_WeaponMaxDist();
void GScr_IsTurretActive();
void GScr_IsWeaponClipOnly();
void GScr_IsWeaponDetonationTimed();
void GScr_GetMoveDelta();
void GScr_GetAngleDelta();
void GScr_GetNorthYaw();
void Scr_SetFxAngles(unsigned int givenAxisCount, float (*axis)[3], float *angles);
void Scr_LoadFX();
void Scr_FxParamError(unsigned int paramIndex, const char *errorString, int fxId);
void Scr_PlayFX();
void Scr_PlayFXOnTag();
void Scr_PlayLoopedFX();
void Scr_SpawnFX();
void Scr_TriggerFX();
void Scr_GetFXVis();
void Scr_PhysicsExplosionSphere();
void Scr_PhysicsRadiusJolt();
void Scr_PhysicsRadiusJitter();
void Scr_PhysicsExplosionCylinder();
void Scr_SetFog(const char *cmd, double start, double density, double r, double g, double b, double time);
void Scr_SetExponentialFog();
void Scr_VisionSetNaked();
void Scr_VisionSetNight();
void Scr_SetCullDist();
void Scr_GetMapSunLight();
void Scr_SetSunLight();
void Scr_ResetSunLight();
void Scr_GetMapSunDirection();
void Scr_SetSunDirection();
void Scr_LerpSunDirection();
void Scr_ResetSunDirection();
void Scr_BadPlace_Delete();
void Scr_BadPlace_Cylinder();
void Scr_BadPlace_Arc();
void Scr_BadPlace_Brush();
void Scr_ClearAllCorpses();
void GScr_GetNumParts();
void GScr_GetPartName();
XAnimTree_s *GScr_GetEntAnimTree(gentity_s *ent);
void G_FlagAnimForUpdate(gentity_s *ent);
void Scr_AnimRelative(scr_entref_t entref);
void ScrCmd_animrelative(scr_entref_t entref);
void DumpAnimCommand(
    const char *funcName,
    XAnimTree_s *tree,
    unsigned int anim,
    int root,
    float weight,
    float time,
    float rate);
void GScr_ClearAnim(scr_entref_t entref);
void GScr_HandleAnimError(int error);
void GScr_SetAnimKnobInternal(scr_entref_t entref, unsigned int flags);
void GScr_SetAnimKnob(scr_entref_t entref);
void GScr_SetAnimKnobLimited(scr_entref_t entref);
void GScr_SetAnimKnobRestart(scr_entref_t entref);
void GScr_SetAnimKnobLimitedRestart(scr_entref_t entref);
void GScr_SetAnimKnobAllInternal(scr_entref_t entref, unsigned int flags);
void GScr_SetAnimKnobAll(scr_entref_t entref);
void GScr_SetAnimKnobAllLimited(scr_entref_t entref);
void GScr_SetAnimKnobAllRestart(scr_entref_t entref);
void GScr_SetAnimKnobAllLimitedRestart(scr_entref_t entref);
void GScr_SetAnimInternal(scr_entref_t entref, unsigned int flags);
void GScr_SetAnim(scr_entref_t entref);
void GScr_SetAnimLimited(scr_entref_t entref);
void GScr_SetAnimRestart(scr_entref_t entref);
void GScr_SetAnimLimitedRestart(scr_entref_t entref);
void GScr_GetAnimTime(scr_entref_t entref);
void GScr_GetAnimAssetType(scr_entref_t entref);
void GScr_SetFlaggedAnimKnobInternal(scr_entref_t entref, unsigned int flags);
void GScr_SetFlaggedAnimKnob(scr_entref_t entref);
void GScr_SetFlaggedAnimKnobLimited(scr_entref_t entref);
void GScr_SetFlaggedAnimKnobRestart(scr_entref_t entref);
void GScr_SetFlaggedAnimKnobLimitedRestart(scr_entref_t entref);
void GScr_SetFlaggedAnimKnobAllInternal(scr_entref_t entref, unsigned int flags, const char *usage);
void GScr_SetFlaggedAnimKnobAll(scr_entref_t entref);
void GScr_SetFlaggedAnimKnobAllRestart(scr_entref_t entref);
void GScr_SetFlaggedAnimInternal(scr_entref_t entref, unsigned int flags);
void GScr_SetFlaggedAnim(scr_entref_t entref);
void GScr_SetFlaggedAnimLimited(scr_entref_t entref);
void GScr_SetFlaggedAnimRestart(scr_entref_t entref);
void GScr_SetFlaggedAnimLimitedRestart(scr_entref_t entref);
void GScr_SetAnimTime(scr_entref_t entref);
void GScr_DumpAnims(scr_entref_t entref);
void GScr_ShellShock(scr_entref_t entref);
void GScr_StopShellShock(scr_entref_t entref);
void GScr_SetDepthOfField(scr_entref_t entref);
void GScr_SetViewModelDepthOfField(scr_entref_t entref);
void GScr_ViewKick(scr_entref_t entref);
void GScr_GetEntnum(scr_entref_t entref);
void GScr_ValidateLightVis(int eType);
void GScr_LockLightVis(scr_entref_t entref);
void GScr_UnlockLightVis(scr_entref_t entref);
void GScr_Launch(scr_entref_t entref);
void GScr_SetSoundBlend(scr_entref_t entref);
void GScr_LocalToWorldCoords(scr_entref_t entref);
void GScr_GetEntityNumber(scr_entref_t entref);
void GScr_EnableGrenadeTouchDamage(scr_entref_t entref);
void GScr_DisableGrenadeTouchDamage(scr_entref_t entref);
void GScr_MissileSetTarget(scr_entref_t entref);
void GScr_EnableAimAssist(scr_entref_t entref);
void GScr_DisableAimAssist(scr_entref_t entref);
void GScr_MakeFakeAI(scr_entref_t entref);
void GScr_SetLookatText(scr_entref_t entref);
void GScr_SetRightArc(scr_entref_t entref);
void GScr_SetLeftArc(scr_entref_t entref);
void GScr_SetTopArc(scr_entref_t entref);
void GScr_SetBottomArc(scr_entref_t entref);
void GScr_SetDefaultDropPitch(scr_entref_t entref);
void GScr_RestoreDefaultDropPitch(scr_entref_t entref);
void GScr_TurretFireDisable(scr_entref_t entref);
void GScr_TurretFireEnable(scr_entref_t entref);
void GScr_SetSpawnerTeam(scr_entref_t entref);
void GScr_PrecacheMenu();
int GScr_GetScriptMenuIndex(const char *pszMenu);
void GScr_SetDebugOrigin();
void GScr_SetDebugAngles();
void GScr_OpenFile();
void GScr_CloseFile();
void Scr_FPrint_internal(bool commaBetweenFields);
void GScr_FPrintln();
void GScr_FPrintFields();
void GScr_FReadLn();
void GScr_FGetArg();
void Scr_PlayRumbleOnPosition_Internal(int event);
gentity_s *Scr_PlayRumbleOnPosition();
gentity_s *Scr_PlayRumbleLoopOnPosition();
gentity_s *Scr_StopAllRumbles();
void ScrCmd_GiveAchievement();
void ScrCmd_UpdateGamerProfile();
void GScr_SetMiniMap();
void GScr_GetArrayKeys();
void GScr_ClearLocalizedStrings();
void Scr_TableLookup();
void Scr_TableLookupIString();
void Scr_GetReflectionLocs();
void Scr_RefreshHudCompass();
void Scr_RefreshHudAmmoCounter();
void Scr_LogString();
void ScrCmd_LogString(scr_entref_t entref);
void(*BuiltIn_GetFunction(const char **pName, int *type))();
void(*Scr_GetFunction(const char **pName, int *type))();
void ScrCmd_PlayRumbleOnEntity_Internal(scr_entref_t entref, int event);
void ScrCmd_PlayRumbleOnEntity(scr_entref_t entref);
void ScrCmd_PlayRumbleLoopOnEntity(scr_entref_t entref);
void ScrCmd_StopRumble(scr_entref_t entref);
void ScrCmd_AddAIEventListener(scr_entref_t entref);
void ScrCmd_RemoveAIEventListener(scr_entref_t entref);
gentity_s *GScr_SetupLightEntity(scr_entref_t entref);
void GScr_GetLightColor(scr_entref_t entref);
void GScr_SetLightColor(scr_entref_t entref);
void GScr_GetLightIntensity(scr_entref_t entref);
void GScr_SetLightIntensity(scr_entref_t entref);
void GScr_GetLightRadius(scr_entref_t entref);
void GScr_SetLightRadius(scr_entref_t entref);
void GScr_GetLightFovInner(scr_entref_t entref);
void GScr_GetLightFovOuter(scr_entref_t entref);
void GScr_SetLightFovRange(scr_entref_t entref);
void GScr_GetLightExponent(scr_entref_t entref);
void GScr_SetLightExponent(scr_entref_t entref);
void GScr_StartRagdoll(scr_entref_t entref);
void GScr_IsRagdoll(scr_entref_t entref);
void(*BuiltIn_GetMethod(const char **pName, int *type))(scr_entref_t);
void(*Scr_GetMethod(const char **pName, int *type))(scr_entref_t __struct_ptr);
void Scr_SetOrigin(gentity_s *ent, int offset);
void Scr_SetAngles(gentity_s *ent, int offset);
void Scr_SetHealth(gentity_s *ent, int offset);
void GScr_Shutdown();
void GScr_SetScriptsAndAnimsForEntities(ScriptFunctions *functions);
void GScr_SetScripts(ScriptFunctions *functions);
void ScrCmd_GetShootAtPosition(scr_entref_t entref);
void ScrCmd_animscriptedInternal(scr_entref_t entref, int bDelayForActor);
void G_StopAnimScripted(gentity_s *ent);
void ScrCmd_stopanimscripted(scr_entref_t entref);
void ScrCmd_animscripted(scr_entref_t entref);
void G_SetAnimTree(gentity_s *ent, scr_animtree_t *animtree);
void GScr_UseAnimTree(scr_entref_t entref);
void GScr_StopUseAnimTree(scr_entref_t entref);

// more functions
void GScr_GetNumVehicles();
void GScr_PrecacheVehicle();


static const char *nodeStringTable[20] =
{
  "BAD NODE",
  "Path",
  "Cover Stand",
  "Cover Crouch",
  "Cover Crouch Window",
  "Cover Prone",
  "Cover Right",
  "Cover Left",
  "Cover Wide Right",
  "Cover Wide Left",
  "Conceal Stand",
  "Conceal Crouch",
  "Conceal Prone",
  "Reacquire",
  "Balcony",
  "Scripted",
  "Begin",
  "End",
  "Turret",
  "Guard"
};


extern scr_data_t g_scr_data;

extern const BuiltinMethodDef methods[104];
extern const BuiltinMethodDef methods_2[166];
extern BuiltinFunctionDef functions[251];
