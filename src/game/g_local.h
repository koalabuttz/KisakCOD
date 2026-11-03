#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <qcommon/qcommon.h>
#include <bgame/bg_local.h>
#include <script/scr_variable.h>
#include "game_public.h"


enum $854C867908149C343981C8BAF4C9A230 : __int32
{
    ENTFIELD_ENTITY = 0x0,
    ENTFIELD_SENTIENT = 0x4000,
    ENTFIELD_ACTOR = 0x8000,
    ENTFIELD_CLIENT = 0xC000,
    ENTFIELD_MASK = 0xC000,
};

// MOD = Means of Death
enum meansOfDeath_t : __int32
{
    MOD_UNKNOWN = 0x0,
    MOD_PISTOL_BULLET = 0x1,
    MOD_RIFLE_BULLET = 0x2,
    MOD_GRENADE = 0x3,
    MOD_GRENADE_SPLASH = 0x4,
    MOD_PROJECTILE = 0x5,
    MOD_PROJECTILE_SPLASH = 0x6,
    MOD_MELEE = 0x7,
    MOD_HEAD_SHOT = 0x8,
    MOD_CRUSH = 0x9,
    MOD_TELEFRAG = 0xA,
    MOD_FALLING = 0xB,
    MOD_SUICIDE = 0xC,
    MOD_TRIGGER_HURT = 0xD,
    MOD_EXPLOSIVE = 0xE,
    MOD_IMPACT = 0xF,
    MOD_NUM = 0x10,
};

struct SpawnFuncEntry
{
    const char *classname;
    void(*callback)(gentity_s *);
};

struct ent_field_t
{
    const char *name;
    int ofs;
    fieldtype_t type;
    void(*callback)(gentity_s *, int);
};

struct animscripted_s
{
    float axis[4][3];
    float originError[3];
    float anglesError[3];
    unsigned __int16 anim;
    unsigned __int16 root;
    unsigned __int8 bStarted;
    unsigned __int8 mode;
    float fHeightOfs;
    float fEndPitch;
    float fEndRoll;
    float fOrientLerp;
};

struct entityHandler_t
{
    void(*think)(gentity_s *);
    void(*reached)(gentity_s *);
    void(*blocked)(gentity_s *, gentity_s *);
    void(*touch)(gentity_s *, gentity_s *, int);
    void(*use)(gentity_s *, gentity_s *, gentity_s *);
    void(*pain)(gentity_s *, gentity_s *, int iDamage, const float * vPoint, const int iMod, const float * vDir, const hitLocation_t hitLoc, const int weaponIdx);
    void(*die)(gentity_s *self, gentity_s *inflictor, gentity_s *attacker, const int iDamage, const int iMod, const int iWeapon, const float * vDir, const hitLocation_t hitLoc);
    void(*entinfo)(gentity_s *, float *);
    void(*controller)(const gentity_s *, int *);
    int methodOfDeath;
    int splashMethodOfDeath;
};

// g_active
void P_DamageFeedback(gentity_s *player);
void G_SetClientSound(gentity_s *ent);
void G_TouchEnts(gentity_s *ent, int numtouch, int *touchents);
void ClientImpacts(gentity_s *ent, pmove_t *pm);
void G_DoTouchTriggers(gentity_s *ent);
void NotifyGrenadePullback(gentity_s *ent, unsigned int weaponIndex);
bool IsLiveGrenade(gentity_s *ent);
void AttemptLiveGrenadePickup(gentity_s *clientEnt);
void ClientEvents(gentity_s *ent, int oldEventSequence);
void Client_ClaimNode(gentity_s *ent);
void G_PlayerStateToEntityStateExtrapolate(playerState_s *ps, entityState_s *s, int time, int snap);
void ClientThink_real(gentity_s *ent);
void ClientThink(int clientNum);
void ClientEndFrame(gentity_s *ent);
gentity_s *G_GetPlayer();
void G_UpdatePlayer(gentity_s *ent);
void G_UpdatePlayerTriggers(gentity_s *ent);



// g_animscripted
void LocalToWorldOriginAndAngles(
    const float (*matrix)[3],
    const float *trans,
    const float *rot,
    float *origin,
    float *angles);
void CalcDeltaOriginAndAngles(
    DObj_s *obj,
    unsigned int anim,
    const float (*matrix)[3],
    float *origin,
    float *angles);
void GetDeltaOriginAndAngles(
    const XAnim_s *anims,
    unsigned int anim,
    const float (*matrix)[3],
    float *trans,
    float *origin,
    float *angles);
void G_Animscripted_DeathPlant(
    gentity_s *ent,
    const XAnim_s *anims,
    unsigned int anim,
    float *origin,
    const float *angles);
void G_AnimScripted_ClearAnimWeights(
    DObj_s *obj,
    XAnimTree_s *pAnimTree,
    unsigned int root,
    struct actor_s *pActor);
void G_Animscripted(
    gentity_s *ent,
    float *origin,
    const float *angles,
    unsigned int anim,
    unsigned int root,
    unsigned int notifyName,
    unsigned __int8 animMode);
void G_ReduceOriginError(float *origin, float *originError, double maxChange);
void G_ReduceAnglesError(float *angles, float *anglesError, double maxChange);
void G_AnimScripted_Think_DeathPlant(gentity_s *ent, XAnimTree_s *tree, float *origin, float *angles);
void G_AnimScripted_UpdateEntityOriginAndAngles(gentity_s *ent, float *origin, const float *angles);
void G_Animscripted_Think(gentity_s *ent);
void GScr_GetStartOrigin();
void GScr_GetStartAngles();
void GScr_GetCycleOriginOffset();


// g_client
void G_FinishSetupSpawnPoint(gentity_s *ent);
void G_SetupSpawnPoint(gentity_s *ent);
void SP_info_player_start(gentity_s *ent);
int SpotWouldTelefrag(gentity_s *spot);
gentity_s *SelectNearestDeathmatchSpawnPoint(const float *from);
gentity_s *SelectRandomDeathmatchSpawnPoint();
gentity_s *SelectSpawnPoint(const float *avoidPoint, float *origin, float *angles);
gentity_s *SelectInitialSpawnPoint(float *origin, float *angles);
void SetClientOrigin(gentity_s *ent, float *origin);
void InitClientDeltaAngles(gclient_s *client);
void SetClientViewAngle(gentity_s *ent, const float *angle);
void G_GetPlayerViewOrigin(const playerState_s *ps, float *origin);
void G_GetPlayerViewDirection(const gentity_s *ent, float *forward, float *right, float *up);
int Client_GetPushed(gentity_s *pSelf, gentity_s *pOther);
void Client_Touch(gentity_s *pSelf, gentity_s *pOther, int bTouched);
void respawn(gentity_s *ent);
char *ClientConnect(int clientNum);
void ClientSpawn(gentity_s *ent);
void HeadHitEnt_Pain(
    gentity_s *pSelf,
    gentity_s *pAttacker,
    int iDamage,
    const float *vPoint,
    int iMod,
    const float *vDir,
    const hitLocation_t hitLoc,
    const int weaponIdx);
void HeadHitEnt_Die(
    gentity_s *self,
    gentity_s *inflictor,
    gentity_s *attacker,
    int damage,
    int meansOfDeath,
    int iWeapon,
    const float *vDir,
    hitLocation_t hitLoc);
void G_UpdateHeadHitEnt(gentity_s *pSelf);
void G_RemoveHeadHitEnt(gentity_s *pSelf);
void ClientBegin(int clientNum);


// g_main
const dvar_s *G_RegisterServerDemoDvars();
void G_RegisterDebugDvars();
void TRACK_g_main();
int G_GetServerSnapTime();
int G_GetTime();
int G_GetSpecialIndex(int entnum);
void G_RegisterDvars();
void G_InitDvars();
void G_FreeEntities();
void *Hunk_AllocActorXAnimServer(int size);
void G_LoadAnimTreeInstances();
void G_FreeAnimTreeInstances();
void G_ClearLowHunk();
bool G_DemoPlaying();
void *Hunk_AllocXAnimServer(int size);
bool G_ExitAfterConnectPaths();
void GScr_LoadScriptsAndAnims();
void ScriptIOFilesInit();
void ScriptIOFilesShutdown();
void G_PrintFastFileErrors(const char *fastfile);
void G_PrintAllFastFileErrors();
void G_InitGame(
    unsigned int randomSeed,
    int restart,
    int checksum,
    int loadScripts,
    int savegame,
    SaveGame **save);
void G_ShutdownGame(int clearScripts);
void G_ChangeLevel();
bool G_IsNextMapWaiting();
void G_SetNextMap(const char *mapname);
void G_LoadNextMap();
void G_CheckReloadStatus();
void G_ApplyEntityEq(gentity_s *ent);
void G_RunThink(gentity_s *ent);
void G_DrawEntityBBoxes();
void DebugDumpAnims();
void DebugDumpAIEventListeners();
void G_CheckLoadGame(int checksum, SaveGame *save);
void G_XAnimUpdateEnt(gentity_s *ent);
void G_ClientDoPerFrameNotifies(gentity_s *ent);
void G_RunFrameForEntityInternal(gentity_s *ent);
void G_RunFrameForEntity(gentity_s *ent);
void G_RunPreFrame();
int G_GetFramePos();
int NotifyTriggers();
void G_SendClientMessages();
void G_ArchiveSpecialEntityInfo(const entityState_s *es, MemoryFile *memFile);
void G_TraceCapsule(
    trace_t *results,
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    int passEntityNum,
    int contentmask);
int G_TraceCapsuleComplete(
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    int passEntityNum,
    int contentmask);
void G_LocationalTrace(
    trace_t *results,
    const float *start,
    const float *end,
    int passEntityNum,
    int contentmask,
    unsigned __int8 *priorityMap);
void G_LocationalTraceAllowChildren(
    trace_t *results,
    const float *start,
    const float *end,
    int passEntityNum,
    int contentmask,
    unsigned __int8 *priorityMap);
int G_LocationalTracePassed(
    const float *start,
    const float *end,
    int passEntityNum,
    int passEntityNum1,
    int contentmask,
    unsigned __int8 *priorityMap);
void G_SightTrace(int *hitNum, const float *start, const float *end, int passEntityNum, int contentmask);
void G_AddDebugString(const float *xyz, const float *color, double scale, const char *pszText);
void G_AddDebugStringWithDuration(
    const float *xyz,
    const float *color,
    double scale,
    const char *pszText,
    int duration);
void ShowEntityInfo();
int G_RunFrame(ServerFrameExtent extent, int timeCap);
void G_LoadLevel();


// g_misc
float G_GetEntInfoScale();
void SP_info_notnull(gentity_s *self);
void SP_light(gentity_s *self);
void SP_info_volume(gentity_s *self);
void TeleportPlayer(gentity_s *player, float *origin, const float *angles);
void SP_sound_blend(gentity_s *self);
gentity_s *G_SpawnSoundBlend();
void G_SetSoundBlend(gentity_s *ent, unsigned __int16 alias0, unsigned __int16 alias1, double lerp);
void G_SetSoundBlendVolumeScale(gentity_s *ent, double scale);
float G_GetSoundBlendVolumeScale(gentity_s *ent);
void EntinfoPosAndScale(gentity_s *self, float *source, float *pos, float *textScale, float *dist);
void misc_EntInfo(gentity_s *self, float *source);
void EntInfo_Item(gentity_s *self, float *source);
int ByteFromFloatColor(float from);


// g_client_script_cmd
int G_GetNeededStartAmmo(gentity_s *pSelf, WeaponDef *weapDef);
void InitializeAmmo(gentity_s *pSelf, int weaponIndex, unsigned __int8 weaponModel, int hadWeapon);
void G_FlushCommandNotifies();
void G_ProcessCommandNotifies();

void PlayerCmd_giveWeapon(scr_entref_t entref);
void PlayerCmd_takeWeapon(scr_entref_t entref);
void PlayerCmd_takeAllWeapons(scr_entref_t entref);
void PlayerCmd_getCurrentWeapon(scr_entref_t entref);
void PlayerCmd_getCurrentWeaponClipAmmo(scr_entref_t entref);
void PlayerCmd_getCurrentOffhand(scr_entref_t entref);
void PlayerCmd_setOffhandSecondaryClass(scr_entref_t entref);
void PlayerCmd_getOffhandSecondaryClass(scr_entref_t entref);
void PlayerCmd_hasWeapon(scr_entref_t entref);
void PlayerCmd_switchToWeapon(scr_entref_t entref);
void PlayerCmd_switchToOffhand(scr_entref_t entref);
void PlayerCmd_giveStartAmmo(scr_entref_t entref);
void PlayerCmd_giveMaxAmmo(scr_entref_t entref);
void PlayerCmd_getFractionStartAmmo(scr_entref_t entref);
void PlayerCmd_getFractionMaxAmmo(scr_entref_t entref);
void PlayerCmd_setOrigin(scr_entref_t entref);
void PlayerCmd_SetVelocity(scr_entref_t entref);
void PlayerCmd_GetVelocity(scr_entref_t entref);
void PlayerCmd_setAngles(scr_entref_t entref);
void PlayerCmd_getAngles(scr_entref_t entref);
void PlayerCmd_getViewHeight(scr_entref_t entref);
void PlayerCmd_getNormalizedMovement(scr_entref_t entref);
void PlayerCmd_useButtonPressed(scr_entref_t entref);
void PlayerCmd_attackButtonPressed(scr_entref_t entref);
void PlayerCmd_adsButtonPressed(scr_entref_t entref);
void PlayerCmd_meleeButtonPressed(scr_entref_t entref);
int PlayerCmd_CheckButtonPressed();
void PlayerCmd_buttonPressed(scr_entref_t entref);
void PlayerCmd_notifyOnCommand(scr_entref_t entref);
void PlayerCmd_playerADS(scr_entref_t entref);
void PlayerCmd_isOnGround(scr_entref_t entref);
void PlayerCmd_SetViewmodel(scr_entref_t entref);
void PlayerCmd_AllowADS(scr_entref_t entref);
void PlayerCmd_AllowJump(scr_entref_t entref);
void PlayerCmd_AllowSprint(scr_entref_t entref);
void PlayerCmd_AllowMelee(scr_entref_t entref);
void PlayerCmd_SetSpreadOverride(scr_entref_t entref);
void PlayerCmd_ResetSpreadOverride(scr_entref_t entref);
void PlayerCmd_ShowViewmodel(scr_entref_t entref);
void PlayerCmd_HideViewmodel(scr_entref_t entref);
void PlayerCmd_AllowStand(scr_entref_t entref);
void PlayerCmd_AllowCrouch(scr_entref_t entref);
void PlayerCmd_AllowProne(scr_entref_t entref);
void PlayerCmd_AllowLean(scr_entref_t entref);
void PlayerCmd_OpenMenu(scr_entref_t entref);
void PlayerCmd_OpenMenuNoMouse(scr_entref_t entref);
void PlayerCmd_CloseMenu(scr_entref_t entref);
void PlayerCmd_FreezeControls(scr_entref_t entref);
void PlayerCmd_SetEQLerp(scr_entref_t entref);
void PlayerCmd_SetEQ(scr_entref_t entref);
void PlayerCmd_DeactivateEq(scr_entref_t entref);
void PlayerCmd_SetReverb(scr_entref_t entref);
void PlayerCmd_DeactivateReverb(scr_entref_t entref);
void PlayerCmd_SetChannelVolumes(scr_entref_t entref);
void PlayerCmd_DeactivateChannelVolumes(scr_entref_t entref);
void ScrCmd_IsLookingAt(scr_entref_t entref);
void PlayerCmd_IsFiring(scr_entref_t entref);
void PlayerCmd_IsThrowingGrenade(scr_entref_t entref);
void PlayerCmd_IsMeleeing(scr_entref_t entref);
void ScrCmd_PlayLocalSound(scr_entref_t entref);
void ScrCmd_StopLocalSound(scr_entref_t entref);
void ScrCmd_SetAutoPickup(scr_entref_t entref);
void PlayerCmd_SetWeaponAmmoClip(scr_entref_t entref);
void PlayerCmd_SetWeaponAmmoStock(scr_entref_t entref);
void PlayerCmd_GetWeaponAmmoClip(scr_entref_t entref);
void PlayerCmd_GetWeaponAmmoStock(scr_entref_t entref);
void PlayerCmd_AnyAmmoForWeaponModes(scr_entref_t entref);
void PlayerCmd_EnableHealthShield(scr_entref_t entref);
void PlayerCmd_SetClientDvar(scr_entref_t entref);
void PlayerCmd_SetClientDvars(scr_entref_t entref);
void PlayerCmd_BeginLocationSelection(scr_entref_t entref);
void PlayerCmd_EndLocationSelection(scr_entref_t entref);
void PlayerCmd_WeaponLockStart(scr_entref_t entref);
void PlayerCmd_WeaponLockFinalize(scr_entref_t entref);
void PlayerCmd_WeaponLockFree(scr_entref_t entref);
void PlayerCmd_WeaponLockTargetTooClose(scr_entref_t entref);
void PlayerCmd_WeaponLockNoClearance(scr_entref_t entref);
void PlayerCmd_SetActionSlot(scr_entref_t entref);
void PlayerCmd_DisableWeapons(scr_entref_t entref);
void PlayerCmd_EnableWeapons(scr_entref_t entref);
void PlayerCmd_NightVisionForceOff(scr_entref_t entref);
void PlayerCmd_GetWeaponsList(scr_entref_t entref);
void PlayerCmd_GetWeaponsListPrimaries(scr_entref_t entref);
void PlayerCmd_EnableInvulnerability(scr_entref_t entref);
void PlayerCmd_DisableInvulnerability(scr_entref_t entref);
void PlayerCmd_ForceViewmodelAnimation(scr_entref_t entref);
void PlayerCmd_DisableTurretDismount(scr_entref_t entref);
void PlayerCmd_EnableTurretDismount(scr_entref_t entref);
void PlayerCmd_UploadScore(scr_entref_t entref);
void PlayerCmd_UploadTime(scr_entref_t entref);
void(*Player_GetMethod(const char **pName))(scr_entref_t);
void G_AddCommandNotify(volatile unsigned __int16 notify);


// g_cmds
int CheatsOkInternal(gentity_s *ent);
int CheatsOk(gentity_s *ent);
char *ConcatArgs(int start);
void SanitizeString(char *in, char *out);
void G_setfog(const char *fogstring);
void Cmd_Fogswitch_f();
void Cmd_SetSoundLength_f();
void Cmd_RemoveCorpse_f();
void Cmd_Give_f(gentity_s *ent);
void Cmd_Take_f(gentity_s *ent);
void Cmd_God_f(gentity_s *ent);
void Cmd_DemiGod_f(gentity_s *ent);
void Cmd_Notarget_f(gentity_s *ent);
void Cmd_Noclip_f(gentity_s *ent);
void Cmd_UFO_f(gentity_s *ent);
void Cmd_Kill_f(gentity_s *ent);
void Cmd_Where_f(gentity_s *ent);
void Cmd_SetViewpos_f(gentity_s *ent);
void Cmd_JumpToNode_f(gentity_s *ent);
void Cmd_InterruptCamera_f(gentity_s *ent);
void Cmd_DropWeapon_f(gentity_s *pSelf);
void Cmd_MenuResponse_f(gentity_s *pEnt);
// attributes: thunk
void Cmd_PrintEntities_f();
void Cmd_VisionSetNaked_f();
void Cmd_VisionSetNight_f();
void ClientCommand(int clientNum, const char *s);


// g_combat
void TRACK_g_combat();
void G_HitLocStrcpy(unsigned __int8 *pMember, const char *pszKeyValue);
void G_ParseHitLocDmgTable();
void TossClientItems(gentity_s *self);
void LookAtKiller(gentity_s *self, gentity_s *inflictor, gentity_s *attacker);
int G_MeansOfDeathFromScriptParam(unsigned int scrParam);
void use_trigger_use(gentity_s *ent, gentity_s *other, gentity_s *activator);
void player_die(
    gentity_s *self,
    gentity_s *inflictor,
    gentity_s *attacker,
    const int damage,
    const int meansOfDeath,
    const int iWeapon,
    const float *vDir,
    const hitLocation_t hitLoc);
float G_GetWeaponHitLocationMultiplier(unsigned int hitLoc, unsigned int weapon);
void handleDeathInvulnerability(gentity_s *targ, int prevHealth, int mod);
void G_DamageNotify(
    unsigned __int16 notify,
    gentity_s *targ,
    gentity_s *attacker,
    const float *dir,
    const float *point,
    int damage,
    int mod,
    unsigned int modelIndex,
    unsigned int partName);
int G_GetWeaponIndexForEntity(const gentity_s *ent);
bool G_ShouldTakeBulletDamage(gentity_s *targ, gentity_s *attacker);
void G_Damage(
    gentity_s *targ,
    gentity_s *inflictor,
    gentity_s *attacker,
    const float *dir,
    const float *point,
    int damage,
    unsigned int dflags,
    int mod,
    int weapon,
    hitLocation_t hitLoc,
    unsigned int modelIndex,
    unsigned int partName);
int G_CanRadiusDamageFromPos(
    gentity_s *targ,
    const float *targetPos,
    gentity_s *inflictor,
    const float *centerPos,
    float radius,
    float coneAngleCos,
    const float *coneDirection,
    float maxHeight,
    bool useEyeOffset,
    int contentMask);
float EntDistToPoint(float *origin, gentity_s *ent);
void GetEntListForRadius(
    const float *origin,
    float radius_max,
    float radius_min,
    int *entList,
    int *entListCount);
void AddScrTeamName(team_t team);
float G_GetRadiusDamageDistanceSquared(float *damageOrigin, gentity_s *ent);
bool G_WithinDamageRadius(float *damageOrigin, double radiusSquared, gentity_s *ent);
bool G_ClientFlashbanged(gclient_s *client);
int G_GetHitLocationString(unsigned int hitLoc);
int G_CanRadiusDamage(
    gentity_s *targ,
    gentity_s *inflictor,
    const float *centerPos,
    double radius,
    double coneAngleCos,
    float *coneDirection,
    int contentMask);
void FlashbangBlastEnt(
    gentity_s *ent,
    float *blastOrigin,
    double radius_max,
    double radius_min,
    gentity_s *attacker,
    team_t team);
void G_FlashbangBlast(
    float *origin,
    double radius_max,
    double radius_min,
    gentity_s *attacker,
    team_t team);
int G_RadiusDamage(
    float *origin,
    gentity_s *inflictor,
    gentity_s *attacker,
    double fInnerDamage,
    double fOuterDamage,
    double radius,
    double coneAngleCos,
    float *coneDirection,
    gentity_s *ignore,
    int mod,
    int weapon);


// g_spawn
int G_LevelSpawnString(const char *key, const char *defaultString, const char **out);
int G_SpawnFloat(const char *key, const char *defaultString, float *out);
int G_SpawnInt(const char *key, const char *defaultString, int *out);
int G_SpawnVector(const char *key, const char *defaultString, float *out);
void Scr_ReadOnlyField(gentity_s *ent, int offset);
unsigned int G_SetEntityScriptVariableInternal(const char *key, const char *value);
void G_SpawnStruct();
void G_DuplicateEntityFields(gentity_s *dest, const gentity_s *source);
void G_DuplicateScriptFields(gentity_s *dest, const gentity_s *source);
const gitem_s *G_GetItemForClassname(const char *classname, unsigned __int8 model);
void(*G_FindSpawnFunc(
    const char *classname,
    const SpawnFuncEntry *spawnFuncArray,
    int spawnFuncCount))(gentity_s *);
void G_PrintBadModelMessage(gentity_s *ent);
int G_CallSpawnEntity(gentity_s *ent);
void GScr_AddFieldsForEntity();
void GScr_AddFieldsForRadiant();
void Scr_FreeEntity(gentity_s *ent);
void Scr_AddEntity(gentity_s *ent);
gentity_s *Scr_GetEntityAllowNull(unsigned int index);
gentity_s *Scr_GetEntity(unsigned int index);
void Scr_FreeHudElem(game_hudelem_s *hud);
void Scr_AddHudElem(game_hudelem_s *hud);
game_hudelem_s *Scr_GetHudElem(unsigned int index);
int Scr_ExecEntThread(gentity_s *ent, int handle, unsigned int paramcount);
void Scr_AddExecEntThread(gentity_s *ent, int handle, unsigned int paramcount);
void Scr_Notify(gentity_s *ent, unsigned __int16 stringValue, unsigned int paramcount);
void Scr_GetGenericEnt(unsigned int offset, unsigned int name);
void Scr_GetEnt();
void Scr_GetGenericEntArray(unsigned int offset, unsigned int name);
void Scr_GetEntArray();
void GScr_SetDynamicEntityField(gentity_s *ent, unsigned int index);
void SP_worldspawn();
void G_LoadStructs();
void G_SetEntityScriptVariable(const char *key, const char *value, gentity_s *ent);
void G_ParseEntityField(const char *key, const char *value, gentity_s *ent, int ignoreModel);
void G_ParseEntityFields(gentity_s *ent, int ignoreModel);
void G_CallSpawn();
void Scr_SetGenericField(unsigned __int8 *b, fieldtype_t type, int ofs);
void Scr_GetGenericField(unsigned __int8 *b, fieldtype_t type, int ofs);
void G_SpawnEntitiesFromString();
int Scr_SetEntityField(unsigned int entnum, unsigned int offset);
int Scr_SetObjectField(unsigned int classnum, unsigned int entnum, int offset);
void Scr_GetEntityField(unsigned int entnum, unsigned int offset);
void Scr_GetObjectField(unsigned int classnum, unsigned int entnum, unsigned int offset);


// g_targets

#define MAX_TARGETS 32

struct target_t
{
    gentity_s *ent;
    float offset[3];
    int materialIndex;
    int offscreenMaterialIndex;
    int flags;
};

struct TargetGlob
{
    target_t targets[MAX_TARGETS];
    unsigned int targetCount;
};

void G_InitTargets();
void G_LoadTargets();
void Scr_Target_SetShader();
void Scr_Target_SetOffscreenShader();
void Scr_Target_GetArray();
int TargetIndex(gentity_s *ent);
void Scr_Target_IsTarget();
void Scr_Target_Set();
bool Targ_Remove(gentity_s *ent);
void Targ_RemoveAll();
void Scr_Target_Remove();
int G_WorldDirToScreenPos(
    const gentity_s *player,
    double fov_x,
    const float *worldDir,
    float *outScreenPos);
int ScrGetTargetScreenPos(float *screenPos);
void Scr_Target_IsInCircle();
void Scr_Target_IsInRect();
void Scr_Target_StartLockOn();
void Scr_Target_ClearLockOn();
int GetTargetIdx(const gentity_s *ent);
int G_TargetGetOffset(const gentity_s *targ, float *result);
int G_TargetAttackProfileTop(const gentity_s *ent);
void Scr_Target_SetAttackMode();
void Scr_Target_SetJavelinOnly();


// g_trigger
void G_Trigger(gentity_s *self, gentity_s *other);
int InitTrigger(gentity_s *self);
void InitTriggerWait(gentity_s *ent, int spawnflag);
void InitSentientTrigger(gentity_s *self);
void multi_trigger(gentity_s *ent, gentity_s *activator);
void Touch_Multi(gentity_s *self, gentity_s *other, int bTouched);
void SP_trigger_multiple(gentity_s *ent);
void SP_trigger_radius(gentity_s *ent);
void SP_trigger_disk(gentity_s *ent);
void Touch_FriendlyChain(gentity_s *self, gentity_s *other, int bTouched);
void SP_trigger_friendlychain(gentity_s *ent);
void hurt_touch(gentity_s *self, gentity_s *other, int bTouched);
void hurt_use(gentity_s *self, gentity_s *other, gentity_s *activator);
void SP_trigger_hurt(gentity_s *self);
void SP_trigger_once(gentity_s *ent);
bool Respond_trigger_damage(gentity_s *trigger, int damageType);
void Activate_trigger_damage(gentity_s *pEnt, gentity_s *pOther, int iDamage, int iMOD);
void Use_trigger_damage(gentity_s *pEnt, gentity_s *pOther, gentity_s *pActivator);
void Pain_trigger_damage(
    gentity_s *pSelf,
    gentity_s *pAttacker,
    int iDamage,
    const float *vPoint,
    int iMod,
    const float *vDir,
    const hitLocation_t hitLoc,
    const int weaponIdx);
void Die_trigger_damage(
    gentity_s *pSelf,
    gentity_s *pInflictor,
    gentity_s *pAttacker,
    int iDamage,
    int iMod,
    int iWeapon,
    const float *vDir,
    const hitLocation_t hitLoc);
void SP_trigger_damage(gentity_s *pSelf);
void G_CheckHitTriggerDamage(
    gentity_s *pActivator,
    const float *vStart,
    const float *vEnd,
    int iDamage,
    unsigned int iMOD);
void G_GrenadeTouchTriggerDamage(
    gentity_s *pActivator,
    const float *vStart,
    const float *vEnd,
    int iDamage,
    int iMOD);
void SP_trigger_lookat(gentity_s *self);


// g_utils
void TRACK_g_utils();
void G_DumpConfigStrings(int start, int max);
int G_FindConfigstringIndex(const char *name, int start, int max, int create, const char *errormsg);
int G_LocalizedStringIndex(const char *string);
int G_MaterialIndex(const char *name);
void G_SetModelIndex(int modelIndex, const char *name);
int G_ModelIndex(const char *name);
XModel *G_GetModel(int index);
bool G_GetModelBounds(int index, float *outMins, float *outMaxs);
int G_XModelBad(int index);
unsigned int G_ModelName(unsigned int index);
void G_EntityCentroidWithBounds(const gentity_s *ent, const float *mins, const float *maxs, float *centroid);
void G_EntityCentroid(const gentity_s *ent, float *centroid);
int G_EffectIndex(const char *name);
int G_ShellShockIndex(const char *name);
unsigned int G_SoundAliasIndexTransientAdvance(unsigned __int16 aliasIndex, int offset);
unsigned int G_SoundAliasIndexTransient(const char *name);
int G_SoundAliasIndexPermanent(const char *name);
int G_RumbleIndex(const char *name);
void G_SetClientDemoTime(int time);
void G_SetClientDemoServerSnapTime(int time);
void G_ClearDemoEntities();
void G_UpdateDemoEntity(entityState_s *es);
unsigned int G_GetEntAnimTreeId(int entnum);
XAnimTree_s *G_GetEntAnimTreeForId(int entnum, unsigned int id);
void G_ShutdownClientDemo();
XAnimTree_s *G_GetEntAnimTree(gentity_s *ent);
void G_CheckDObjUpdate(gentity_s *ent);
void G_SetModel(gentity_s *ent, const char *modelName);
// attributes: thunk
void G_ReplaceModel_FastFile(const char *originalName, const char *replacementName);
void G_OverrideModel(unsigned int modelindex, const char *defaultModelName);
void G_PrecacheDefaultModels();
int G_EntIsLinkedTo(gentity_s *ent, gentity_s *parent);
void G_UpdateViewAngleClamp(gclient_s *client, const float *worldAnglesCenter);
void G_UpdateGroundTilt(gclient_s *client);
bool G_SlideMove(
    float deltaT,
    float *origin,
    float *velocity,
    float *mins,
    const float *maxs,
    const float *gravity,
    unsigned __int8 passEntityNum,
    int clipMask);
void G_StepSlideMove(
    float deltaT,
    float *origin,
    float *velocity,
    float *mins,
    const float *maxs,
    const float *gravity,
    unsigned __int8 passEntityNum,
    int clipMask);
void G_SafeDObjFree(gentity_s *ent);
int G_DObjUpdateServerTime(gentity_s *ent, int bNotify);
void G_DObjCalcPose(gentity_s *ent, int *partBits);
void G_DObjCalcBone(const gentity_s *ent, int boneIndex);
struct DObjAnimMat *G_DObjGetLocalBoneIndexMatrix(const gentity_s *ent, int boneIndex);
void G_DObjGetWorldBoneIndexMatrix(const gentity_s *ent, int boneIndex, float (*tagMat)[3]);
void G_DObjGetWorldBoneIndexPos(const gentity_s *ent, int boneIndex, float *pos);
struct DObjAnimMat *G_DObjGetLocalTagMatrix(const gentity_s *ent, unsigned int tagName);
int G_DObjGetWorldTagMatrix(const gentity_s *ent, unsigned int tagName, float (*tagMat)[3]);
int G_DObjGetWorldTagPos(const gentity_s *ent, unsigned int tagName, float *pos);
void G_DObjGetWorldTagPos_CheckTagExists(const gentity_s *ent, unsigned int tagName, float *pos);
gentity_s *G_Find(gentity_s *from, int fieldofs, unsigned __int16 match);
void G_InitGentity(gentity_s *e);
void G_PrintEntities();
gentity_s *G_Spawn();
void G_FreeEntityRefs(gentity_s *ed);
void G_FreeAllEntityRefs();
void G_FreeEntityDelay(gentity_s *ed);
void G_BroadcastEntity(gentity_s *ent);
void G_FreeEntityAfterEvent(gentity_s *ent);
int G_SaveFreeEntities(unsigned __int8 *buf);
void G_LoadFreeEntities(unsigned __int8 *buf);
void G_AddPredictableEvent(gentity_s *ent, entity_event_t event, unsigned int eventParm);
void G_AddEvent(gentity_s *ent, unsigned int event, unsigned int eventParm);
void G_RegisterSoundWait(gentity_s *ent, unsigned __int16 index, unsigned int notifyString, int stoppable);
void G_PlaySoundAliasWithNotify(
    gentity_s *ent,
    unsigned __int16 index,
    unsigned int notifyString,
    int stoppable,
    unsigned int event,
    unsigned int notifyevent);
void G_PlaySoundAlias(gentity_s *ent, unsigned __int16 index);
void G_SetOrigin(gentity_s *ent, float *origin);
void G_SetAngle(gentity_s *ent, float *angle);
void G_SetConstString(unsigned __int16 *to, const char *from);
const char *G_GetEntityTypeName(const gentity_s *ent);
void G_SetPM_MPViewer(bool setting);
void G_srand(unsigned int seed);
unsigned int G_GetRandomSeed();
unsigned int G_rand();
float G_flrand(float min, float max);
int G_irand(int min, int max);
float G_random();
float G_crandom();
void G_CalcTagParentAxis(gentity_s *ent, float (*parentAxis)[3]);
void G_CalcTagParentRelAxis(gentity_s *ent, float (*parentRelAxis)[3]);
void G_CalcTagAxis(gentity_s *ent, int bAnglesOnly);
void G_SetFixedLink(gentity_s *ent, unsigned int eAngles);
void G_SetPlayerFixedLink(gentity_s *ent);
void G_GeneralLink(gentity_s *ent);
gentity_s *G_TempEntity(float *origin, int event);
void G_PlaySoundAliasAtPoint(float *origin, unsigned __int16 index);
void G_EntUnlink(gentity_s *ent);
void G_UpdateTagInfo(gentity_s *ent, int bParentHasDObj);
void G_UpdateTagInfoOfChildren(gentity_s *parent, int bHasDObj);
void G_EntUnlinkFree(gentity_s *ent);
void G_FreeEntity(gentity_s *ed);
void G_UpdateTags(gentity_s *ent, int bHasDObj);
void G_DObjUpdate(gentity_s *ent);
int G_EntDetach(gentity_s *ent, const char *modelName, unsigned int tagName);
void G_EntDetachAll(gentity_s *ent);
int G_EntLinkToInternal(gentity_s *ent, gentity_s *parent, unsigned int tagName);
int G_EntLinkTo(gentity_s *ent, gentity_s *parent, unsigned int tagName);
int G_EntLinkToWithOffset(
    gentity_s *ent,
    gentity_s *parent,
    unsigned int tagName,
    float *originOffset,
    const float *anglesOffset);
int G_EntAttach(gentity_s *ent, const char *modelName, unsigned int tagName, int ignoreCollision);


extern bool g_godModeRemoteInputValid;
extern unsigned __int16 *modNames[MOD_NUM];

extern TargetGlob targGlob;