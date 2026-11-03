#pragma once

#include <universal/layout_assert.h>
#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <xanim/xanim.h>
#include <qcommon/msg_mp.h>
#include <bgame/bg_public.h>
#include <script/scr_variable.h>
#include "g_main_mp.h"
#include <game/game_public.h>
#include <script/scr_const.h>
#include <bgame/bg_local.h>

struct game_hudelem_s;
struct weaponParms;

static const char *g_dedicatedEnumNames[4] = { "listen server", "dedicated LAN server", "dedicated internet server", NULL }; // idb

struct BuiltinFunctionDef // sizeof=0xC
{
    const char *actionString;
    void(*actionFunc)();
    int32_t type;
};
static_assert(LAYOUT_32BIT(sizeof(BuiltinFunctionDef) == 0xC));

struct BuiltinMethodDef // sizeof=0xC
{                                       // ...
    const char *actionString;           // ...
    void(*actionFunc)(scr_entref_t); // ...
    int32_t type;                           // ...
};
static_assert(LAYOUT_32BIT(sizeof(BuiltinMethodDef) == 0xC));

enum meansOfDeath_t : int32_t
{                                       // ...
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

enum HELICOPTER_STAGES : __int32
{
    HELICOPTER_ONFIRE = 0x0,
    HELICOPTER_HEAVYSMOKE = 0x1,
    HELICOPTER_SMOKING = 0x2,
    HELICOPTER_FULLHEALTH = 0x3,
    NUM_HELICOPTERSTAGES = 0x4,
};

static uint16_t *modNames[MOD_NUM] =
{
    &scr_const.mod_unknown,
    &scr_const.mod_pistol_bullet,
    &scr_const.mod_rifle_bullet,
    &scr_const.mod_grenade,
    &scr_const.mod_grenade_splash,
    &scr_const.mod_projectile,
    &scr_const.mod_projectile_splash,
    &scr_const.mod_melee,
    &scr_const.mod_head_shot,
    &scr_const.mod_crush,
    &scr_const.mod_telefrag,
    &scr_const.mod_falling,
    &scr_const.mod_suicide,
    &scr_const.mod_trigger_hurt,
    &scr_const.mod_explosive,
    &scr_const.mod_impact
};

struct gameTypeScript_t // sizeof=0x84
{                                       // ...
    char pszScript[64];
    char pszName[64];                   // ...
    int32_t bTeamBased;
};
static_assert(sizeof(gameTypeScript_t) == 0x84);

struct scr_data_t_s // sizeof=0x10A8
{                                       // ...
    int32_t main;                           // ...
    int32_t startupgametype;                // ...
    int32_t playerconnect;                  // ...
    int32_t playerdisconnect;               // ...
    int32_t playerdamage;                   // ...
    int32_t playerkilled;                   // ...
    int32_t votecalled;
    int32_t playervote;
    int32_t playerlaststand;                // ...
    int32_t iNumGameTypes;                  // ...
    gameTypeScript_t list[32];          // ...
};
static_assert(sizeof(scr_data_t_s) == 0x10A8);

struct corpseInfo_t // sizeof=0x4DC
{                                       // ...
    XAnimTree_s *tree;                  // ...
    int32_t entnum;                         // ...
    int32_t time;
    clientInfo_t ci;
    bool falling;
    // padding byte
    // padding byte
    // padding byte
};
static_assert(LAYOUT_32BIT(sizeof(corpseInfo_t) == 0x4DC));

struct scr_data_t // sizeof=0x379C
{                                       // ...
    int32_t levelscript;                    // ...
    int32_t gametypescript;
    scr_data_t_s gametype; // ...
    int32_t delete_;                        // ...
    int32_t initstructs;                    // ...
    int32_t createstruct;                   // ...
    corpseInfo_t playerCorpseInfo[8];   // ...
};
static_assert(LAYOUT_32BIT(sizeof(scr_data_t) == 0x379C));

// g_active_mp
void P_DamageFeedback(gentity_s *player);
void G_SetClientSound(gentity_s *ent);
void ClientImpacts(gentity_s *ent, pmove_t *pm);
void G_TouchTriggers(gentity_s *ent);
void SpectatorThink(gentity_s *ent, usercmd_s *ucmd);
int32_t ClientInactivityTimer(gclient_s *client);
void ClientIntermissionThink(gentity_s *ent);
void NotifyGrenadePullback(gentity_s *ent, uint32_t weaponIndex);
void HandleClientEvent(gclient_s *client, gentity_s *ent, int32_t event, int32_t eventParm);
void AttemptLiveGrenadePickup(gentity_s *clientEnt);
bool IsLiveGrenade(gentity_s *ent);
void ClientEvents(gentity_s *ent, int32_t oldEventSequence);
void G_SetLastServerTime(int32_t clientNum, int32_t lastServerTime);
void G_SetClientContents(gentity_s *pEnt);
void ClientThink_real(gentity_s *ent, usercmd_s *ucmd);
void G_PlayerStateToEntityStateExtrapolate(playerState_s *ps, entityState_s *s, int32_t time, int32_t snap);
void G_AddPlayerMantleBlockage(float *endPos, int32_t duration, pmove_t *pm);
void ClientThink(int32_t clientNum);
void G_RunClient(gentity_s *ent);
void IntermissionClientEndFrame(gentity_s *ent);
void SpectatorClientEndFrame(gentity_s *ent);
bool G_ClientCanSpectateTeam(gclient_s *client, team_t team);
int32_t GetFollowPlayerState(int32_t clientNum, playerState_s *ps);
int32_t StuckInClient(gentity_s *self);
void G_PlayerController(const gentity_s *self, int32_t *partBits);
void ClientEndFrame(gentity_s *ent);
int32_t G_UpdateClientInfo(gentity_s *ent);
void G_PlayerEvent(int32_t clientNum, int32_t event);
int32_t GetFollowPlayerState(int32_t clientNum, playerState_s *ps);



// g_client_mp
void SetClientViewAngle(gentity_s *ent, const float *angle);
void G_GetPlayerViewOrigin(const playerState_s *ps, float *origin);
void ClientUserinfoChanged(uint32_t clientNum);
void ClientCleanName(const char *in, char *out, int32_t outSize);
char *ClientConnect(uint32_t clientNum, uint16_t scriptPersId);
void ClientClearFields(gclient_s *client);
void ClientBegin(int32_t clientNum);
void ClientSpawn(gentity_s *ent, const float *spawn_origin, const float *spawn_angles);
void ClientDisconnect(int32_t clientNum);
uint32_t G_GetNonPVSPlayerInfo(gentity_s *pSelf, float *vPosition, int32_t iLastUpdateEnt);



// g_client_script_cmd_mp
void PlayerCmd_giveWeapon(scr_entref_t entref);
void G_InitializeAmmo(gentity_s *pSelf, int32_t weaponIndex, uint8_t weaponModel, int32_t hadWeapon);
int32_t G_GetNeededStartAmmo(gentity_s *pSelf, WeaponDef *weapDef);
void PlayerCmd_takeWeapon(scr_entref_t entref);
void PlayerCmd_takeAllWeapons(scr_entref_t entref);
void PlayerCmd_getCurrentWeapon(scr_entref_t entref);
bool ClientPlaying(gentity_s *pSelf);
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
void PlayerCmd_GetVelocity(scr_entref_t entref);
void PlayerCmd_setAngles(scr_entref_t entref);
void PlayerCmd_getAngles(scr_entref_t entref);
void PlayerCmd_useButtonPressed(scr_entref_t entref);
void PlayerCmd_attackButtonPressed(scr_entref_t entref);
void PlayerCmd_adsButtonPressed(scr_entref_t entref);
void PlayerCmd_meleeButtonPressed(scr_entref_t entref);
void PlayerCmd_fragButtonPressed(scr_entref_t entref);
void PlayerCmd_secondaryOffhandButtonPressed(scr_entref_t entref);
void PlayerCmd_buttonPressedDEVONLY(scr_entref_t entref);
void PlayerCmd_playerADS(scr_entref_t entref);
void PlayerCmd_isOnGround(scr_entref_t entref);
void PlayerCmd_pingPlayer(scr_entref_t entref);
void PlayerCmd_SetViewmodel(scr_entref_t entref);
void PlayerCmd_GetViewmodel(scr_entref_t entref);
void PlayerCmd_showScoreboard(scr_entref_t entref);
void PlayerCmd_setSpawnWeapon(scr_entref_t entref);
void PlayerCmd_dropItem(scr_entref_t entref);
void PlayerCmd_finishPlayerDamage(scr_entref_t entref);
bool IsBulletImpactMOD(meansOfDeath_t mod);
void PlayerCmd_Suicide(scr_entref_t entref);
void PlayerCmd_OpenMenu(scr_entref_t entref);
void PlayerCmd_OpenMenuNoMouse(scr_entref_t entref);
void PlayerCmd_CloseMenu(scr_entref_t entref);
void PlayerCmd_CloseInGameMenu(scr_entref_t entref);
void PlayerCmd_SetWeaponAmmoClip(scr_entref_t entref);
void PlayerCmd_SetWeaponAmmoStock(scr_entref_t entref);
void PlayerCmd_GetWeaponAmmoClip(scr_entref_t entref);
void PlayerCmd_GetWeaponAmmoStock(scr_entref_t entref);
void PlayerCmd_AnyAmmoForWeaponModes(scr_entref_t entref);
void iclientprintln(scr_entref_t entref);
void iclientprintlnbold(scr_entref_t entref);
void PlayerCmd_spawn(scr_entref_t entref);
void PlayerCmd_setEnterTime(scr_entref_t entref);
void BodyEnd(gentity_s *ent);
void PlayerCmd_ClonePlayer(scr_entref_t entref);
void PlayerCmd_SetClientDvar(scr_entref_t entref);
void PlayerCmd_SetClientDvars(scr_entref_t entref);
void PlayerCmd_IsTalking(scr_entref_t entref);
void PlayerCmd_FreezeControls(scr_entref_t entref);
void PlayerCmd_DisableWeapons(scr_entref_t entref);
void PlayerCmd_EnableWeapons(scr_entref_t entref);
void PlayerCmd_SetReverb(scr_entref_t entref);
void PlayerCmd_DeactivateReverb(scr_entref_t entref);
void PlayerCmd_SetChannelVolumes(scr_entref_t entref);
void PlayerCmd_DeactivateChannelVolumes(scr_entref_t entref);
void ScrCmd_PlayLocalSound(scr_entref_t entref);
void ScrCmd_StopLocalSound(scr_entref_t entref);
void PlayerCmd_SayAll(scr_entref_t entref);
void PlayerCmd_SayTeam(scr_entref_t entref);
void PlayerCmd_AllowADS(scr_entref_t entref);
void PlayerCmd_AllowJump(scr_entref_t entref);
void PlayerCmd_AllowSprint(scr_entref_t entref);
void PlayerCmd_SetSpreadOverride(scr_entref_t entref);
void PlayerCmd_ResetSpreadOverride(scr_entref_t entref);
void PlayerCmd_AllowSpectateTeam(scr_entref_t entref);
void PlayerCmd_GetGuid(scr_entref_t entref);
void PlayerCmd_GetXuid(scr_entref_t entref);
void PlayerCmd_BeginLocationSelection(scr_entref_t entref);
void PlayerCmd_EndLocationSelection(scr_entref_t entref);
void PlayerCmd_SetActionSlot(scr_entref_t entref);
void PlayerCmd_GetWeaponsList(scr_entref_t entref);
void PlayerCmd_GetWeaponsListPrimaries(scr_entref_t entref);
void PlayerCmd_SetPerk(scr_entref_t entref);
void BG_SetPerk(int32_t *perks, uint32_t perkIndex);
void PlayerCmd_HasPerk(scr_entref_t entref);
void PlayerCmd_UnsetPerk(scr_entref_t entref);
void BG_UnsetPerk(int32_t *perks, uint32_t perkIndex);
void PlayerCmd_ClearPerks(scr_entref_t entref);
void PlayerCmd_UpdateScores(scr_entref_t entref);
void PlayerCmd_UpdateDMScores(scr_entref_t entref);
void PlayerCmd_SetRank(scr_entref_t entref);
void(*Player_GetMethod(const char **pName))(scr_entref_t);



// g_cmds_mp
void SendScoreboard(gentity_s *ent);
void Cmd_Score_f(gentity_s *ent);
int32_t CheatsOk(gentity_s *ent);
char *ConcatArgs(int32_t start);
int32_t SV_Cmd_Argc();
void G_setfog(const char *fogstring);
void Cmd_Give_f(gentity_s *ent);
void Cmd_Take_f(gentity_s *ent);
void Cmd_God_f(gentity_s *ent);
void Cmd_DemiGod_f(gentity_s *ent);
void Cmd_Notarget_f(gentity_s *ent);
void Cmd_Noclip_f(gentity_s *ent);
void Cmd_UFO_f(gentity_s *ent);
void Cmd_Kill_f(gentity_s *ent);
void StopFollowing(gentity_s *ent);
int32_t Cmd_FollowCycle_f(gentity_s *ent, int32_t dir);
bool G_IsPlaying(gentity_s *ent);
void G_Say(gentity_s *ent, gentity_s *target, int32_t mode, char *chatText);
void G_SayTo(
    gentity_s *ent,
    gentity_s *other,
    int32_t mode,
    int32_t color,
    const char *teamString,
    const char *cleanname,
    const char *message);
void Cmd_Where_f(gentity_s *ent);
void Cmd_CallVote_f(gentity_s *ent);
void Cmd_Vote_f(gentity_s *ent);
void Cmd_SetViewpos_f(gentity_s *ent);
void Cmd_EntityCount_f();
void Cmd_MenuResponse_f(gentity_s *pEnt);
void ClientCommand(int32_t clientNum);
void Cmd_Say_f(gentity_s *ent, int32_t mode, int32_t arg0);
void Cmd_PrintEntities_f();
void Cmd_VisionSetNaked_f();
const char *SV_Cmd_Argv(int32_t argIndex);
void Cmd_VisionSetNight_f();



// g_combat_mp
void TRACK_g_combat();
void G_ParseHitLocDmgTable();
void LookAtKiller(gentity_s *self, gentity_s *inflictor, gentity_s *attacker);
int32_t G_MeansOfDeathFromScriptParam(uint32_t scrParam);
void player_die(
    gentity_s *self,
    gentity_s *inflictor,
    gentity_s *attacker,
    int32_t damage,
    int32_t meansOfDeath,
    int32_t iWeapon,
    const float *vDir,
    hitLocation_t hitLoc,
    int32_t psTimeOffset);
void DeathGrenadeDrop(gentity_s *self, int32_t meansOfDeath);
double G_GetWeaponHitLocationMultiplier(hitLocation_t hitLoc, uint32_t weapon);
void G_DamageClient(
    gentity_s *targ,
    gentity_s *inflictor,
    gentity_s *attacker,
    const float *dir,
    const float *point,
    int32_t damage,
    int32_t dflags,
    uint32_t mod,
    uint32_t weapon,
    hitLocation_t hitLoc,
    int32_t timeOffset);
uint32_t G_GetWeaponIndexForEntity(const gentity_s *ent);
void G_Damage(
    gentity_s *targ,
    gentity_s *inflictor,
    gentity_s *attacker,
    float *dir,
    float *point,
    int32_t damage,
    int32_t dFlags,
    int32_t mod,
    uint32_t weapon,
    hitLocation_t hitLoc,
    uint32_t modelIndex,
    uint32_t partName,
    int32_t timeOffset);
void DamageNotify(
    uint16_t notify,
    gentity_s *targ,
    gentity_s *attacker,
    float *dir,
    float *point,
    int32_t damage,
    int32_t mod,
    int32_t dFlags,
    uint32_t modelIndex,
    uint32_t partName);
double CanDamage(
    gentity_s *targ,
    gentity_s *inflictor,
    float *centerPos,
    float coneAngleCos,
    float *coneDirection,
    int32_t contentMask);
void G_FlashbangBlast(float *origin, float radius_max, float radius_min, gentity_s *attacker, team_t team);
void GetEntListForRadius(
    const float *origin,
    float radius_max,
    float radius_min,
    int32_t *entList,
    int32_t *entListCount);
void FlashbangBlastEnt(
    gentity_s *ent,
    float *blastOrigin,
    float radius_max,
    float radius_min,
    gentity_s *attacker,
    team_t team);

double EntDistToPoint(const float *origin, gentity_s *ent);
void AddScrTeamName(team_t team);
bool G_WithinDamageRadius(const float *damageOrigin, float radiusSquared, gentity_s *ent);
double G_GetRadiusDamageDistanceSquared(const float *damageOrigin, gentity_s *ent);
int32_t G_RadiusDamage(
    float *origin,
    gentity_s *inflictor,
    gentity_s *attacker,
    float fInnerDamage,
    float fOuterDamage,
    float radius,
    float coneAngleCos,
    float *coneDirection,
    gentity_s *ignore,
    int32_t mod,
    uint32_t weapon);
uint16_t G_GetHitLocationString(hitLocation_t hitLoc);
int32_t G_GetHitLocationIndexFromString(uint16_t sString);




// g_misc_mp
void SP_info_notnull(gentity_s *self);
void SP_light(gentity_s *self);
void TeleportPlayer(gentity_s *player, float *origin, float *angles);
void G_InitTurrets();
void G_ClientStopUsingTurret(gentity_s *self);
void turret_think_client(gentity_s *self);
void turret_track(gentity_s *self, gentity_s *other);
void G_PlayerTurretPositionAndBlend(gentity_s *ent, gentity_s *pTurretEnt);
void turret_clientaim(gentity_s *self, gentity_s *other);
void turret_shoot_internal(gentity_s *self, gentity_s *other);
void Fire_Lead(gentity_s *ent, gentity_s *activator);
void Turret_FillWeaponParms(gentity_s *ent, gentity_s *activator, weaponParms *wp);
void turret_UpdateSound(gentity_s *self);
void turret_think(gentity_s *self);
int32_t turret_ReturnToDefaultPos(gentity_s *self, int32_t bManned);
int32_t turret_UpdateTargetAngles(gentity_s *self, float *desiredAngles, int32_t bManned);
void turret_think_init(gentity_s *self);
void turret_controller(const gentity_s *self, int32_t *partBits);
void G_FreeTurret(gentity_s *self);
bool G_IsTurretUsable(gentity_s *self, gentity_s *owner);
bool turret_behind(gentity_s *self, gentity_s *other);
void turret_use(gentity_s *self, gentity_s *owner, gentity_s *activator);
void G_SpawnTurret(gentity_s *self, const char *weaponinfoname);
void SP_turret(gentity_s *self);




// g_player_corpse_mp
int32_t G_GetPlayerCorpseIndex(gentity_s *ent);
int32_t G_GetFreePlayerCorpseIndex();
void PlayerCorpse_Free(gentity_s *ent);
void G_RunCorpseMove(gentity_s *ent);
void G_BounceCorpse(gentity_s *ent, corpseInfo_t *corpseInfo, trace_t *trace, float *endpos);
char G_GetAnimDeltaForCorpse(gentity_s *ent, float *originChange);
void G_RunCorpse(gentity_s *ent);
void G_RunCorpseAnimate(gentity_s *ent);



// g_scr_helicopter
void CMD_Heli_FreeHelicopter(scr_entref_t entref);
void CMD_Heli_SetDamageStage(scr_entref_t entref);
void(*Helicopter_GetMethod(const char **pName))(scr_entref_t);
void Helicopter_RegisterDvars();
void G_SpawnHelicopter(gentity_s *ent, gentity_s *owner, const char *vehicleInfoName, char *modelName);
void Heli_InitFirstThink(gentity_s *pSelf);
void Helicopter_Pain(
    gentity_s *pSelf,
    gentity_s *pAttacker,
    int32_t damage,
    const float *point,
    const int32_t mod,
    const float *dir,
    const hitLocation_t hitLoc,
    const int32_t weaponIdx);
void Helicopter_Die(
    gentity_s *pSelf,
    gentity_s *pInflictor,
    gentity_s *pAttacker,
    const int32_t damage,
    const int32_t mod,
    const int32_t weapon,
    const float *dir,
    const hitLocation_t hitLoc,
    int32_t psTimeOffset);
void Helicopter_Controller(const gentity_s *pSelf, int32_t *partBits);
void Helicopter_Think(gentity_s *ent);




// g_scr_main_mp
uint32_t GScr_AllocString(const char *s);
void TRACK_g_scr_main();
void Scr_LoadLevel();
void GScr_LoadGameTypeScript();
int32_t GScr_LoadScriptAndLabel(const char *filename, const char *label, int32_t bEnforceExists);
void GScr_LoadScripts();
int32_t GScr_LoadLevelScript();
void GScr_PostLoadScripts();
void GScr_FreeScripts();
void ScrCmd_GetClanId(scr_entref_t entref);
void ScrCmd_GetClanName(scr_entref_t entref);
void GScr_CreatePrintChannel();
void GScr_printChannelSet();
void print();
void println();
void Scr_LocalizationError(uint32_t iParm, const char *pszErrorMessage);
void Scr_ConstructMessageString(
    int32_t firstParmIndex,
    int32_t lastParmIndex,
    const char *errorContext,
    char *string,
    uint32_t stringLimit);
void Scr_ValidateLocalizedStringRef(uint32_t parmIndex, const char *token, int32_t tokenLen);
void Scr_MakeGameMessage(int32_t iClientNum, const char *pszCmd);
void Scr_VerifyWeaponIndex(int32_t weaponIndex, const char *weaponName);
void iprintln();
void iprintlnbold();
void GScr_print3d();
void GScr_line();
void assertCmd();
void assertexCmd();
void assertmsgCmd();
void GScr_IsDefined();
void GScr_IsString();
void GScr_IsAlive();
void GScr_GetDvar();
void GScr_GetDvarInt();
void GScr_GetDvarFloat();
void GScr_SetDvar();
void GScr_GetTime();
void Scr_GetEntByNum();
void Scr_GetWeaponModel();
void GScr_GetAmmoCount(scr_entref_t entref);
gentity_s *GetPlayerEntity(scr_entref_t entref);
gentity_s *GetEntity(scr_entref_t entref);
void GScr_AnimHasNotetrack();
void GScr_GetNotetrackTimes();
void GScr_GetBrushModelCenter();
void GScr_Spawn();
void GScr_SpawnPlane();
void GScr_SpawnTurret();
void GScr_SpawnHelicopter();
uint32_t GScr_PrecacheTurret();
void ScrCmd_SetMoveSpeedScale(scr_entref_t entref);
void ScrCmd_attach(scr_entref_t entref);
void ScrCmd_detach(scr_entref_t entref);
void ScrCmd_detachAll(scr_entref_t entref);
void ScrCmd_GetAttachSize(scr_entref_t entref);
void ScrCmd_GetAttachModelName(scr_entref_t entref);
void ScrCmd_GetAttachTagName(scr_entref_t entref);
void ScrCmd_GetAttachIgnoreCollision(scr_entref_t entref);
void G_EntityStateSetPartBits(gentity_s *ent, const uint32_t *partBits);
void G_EntityStateGetPartBits(const gentity_s *ent, uint32_t *partBits);
void ScrCmd_hidepart(scr_entref_t entref);
void ScrCmd_showpart(scr_entref_t entref);
void ScrCmd_showallparts(scr_entref_t entref);
void ScrCmd_LinkTo(scr_entref_t entref);
void ScrCmd_Unlink(scr_entref_t entref);
void ScrCmd_EnableLinkTo(scr_entref_t entref);
void ScrCmd_GetOrigin(scr_entref_t entref);
void ScrCmd_GetEye(scr_entref_t entref);
void ScrCmd_UseBy(scr_entref_t entref);
void ScrCmd_IsTouching(scr_entref_t entref);
void ScrCmd_SoundExists();
void ScrCmd_PlaySound(scr_entref_t entref);
gentity_s *StartScriptPlaySoundOnEnt(scr_entref_t entref);
void ScrCmd_PlaySoundToTeam(scr_entref_t entref);
void ScrCmd_PlaySoundToPlayer(scr_entref_t entref);
void ScrCmd_PlaySoundAsMaster(scr_entref_t entref);
void ScrCmd_PlayLoopSound(scr_entref_t entref);
void ScrCmd_StopLoopSound(scr_entref_t entref);
void ScrCmd_Delete(scr_entref_t entref);
void ScrCmd_SetModel(scr_entref_t entref);
void ScrCmd_GetNormalHealth(scr_entref_t entref);
void ScrCmd_SetNormalHealth(scr_entref_t entref);
void ScrCmd_Show(scr_entref_t entref);
void ScrCmd_Hide(scr_entref_t entref);
void ScrCmd_LaserOn(scr_entref_t entref);
void ScrCmd_LaserOff(scr_entref_t entref);
void ScrCmd_ShowToPlayer(scr_entref_t entref);
void ScrCmd_SetContents(scr_entref_t entref);
void ScrCmd_GetStance(scr_entref_t entref);
void Scr_SetStableMissile(scr_entref_t entref);
void GScr_SetCursorHint(scr_entref_t entref);
int32_t G_GetHintStringIndex(int32_t *piIndex, char *pszString);
void GScr_SetHintString(scr_entref_t entref);
void GScr_UseTriggerRequireLookAt(scr_entref_t entref);
void GScr_GetEntityNumber(scr_entref_t entref);
void GScr_EnableGrenadeTouchDamage(scr_entref_t entref);
void GScr_DisableGrenadeTouchDamage(scr_entref_t entref);
void GScr_MissileSetTarget(scr_entref_t entref);
void GScr_EnableAimAssist(scr_entref_t entref);
void GScr_DisableAimAssist(scr_entref_t entref);
void G_InitObjectives();
int32_t ObjectiveStateIndexFromString(objectiveState_t *piStateIndex, uint32_t stateString);
void ClearObjective(objective_t *obj);
void Scr_Objective_Add();
void ClearObjective_OnEntity(objective_t *obj);
void SetObjectiveIcon(objective_t *obj, uint32_t paramNum);
void Scr_Objective_Delete();
void Scr_Objective_State();
void Scr_Objective_Icon();
void Scr_Objective_Position();
objective_t *Scr_Objective_OnEntity();
void Scr_Objective_Current();
void GScr_Objective_Team();
void GScr_LogPrint();
void GScr_WorldEntNumber();
int32_t GScr_Obituary();
void GScr_positionWouldTelefrag();
void GScr_getStartTime();
void GScr_PrecacheMenu();
int32_t GScr_GetScriptMenuIndex(const char *pszMenu);
void GScr_PrecacheStatusIcon();
int32_t GScr_GetStatusIconIndex(const char *pszIcon);
void GScr_PrecacheHeadIcon();
int32_t GScr_GetHeadIconIndex(const char *pszIcon);
void GScr_WeaponClipSize();
void GScr_WeaponIsSemiAuto();
void GScr_WeaponIsBoltAction();
void GScr_WeaponType();
void GScr_WeaponClass();
void GScr_WeaponInventoryType();
void GScr_WeaponStartAmmo();
void GScr_WeaponMaxAmmo();
void GScr_WeaponAltWeaponName();
void GScr_WeaponFireTime();
void GScr_IsWeaponClipOnly();
void GScr_IsWeaponDetonationTimed();
void GScr_PrecacheLocationSelector();
int32_t GScr_GetLocSelIndex(const char *mtlName);
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
void GScr_abs();
void GScr_min();
void GScr_max();
void GScr_floor();
void GScr_ceil();
void GScr_sqrt();
void GScr_CastInt();
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
void Scr_MusicPlay();
void Scr_MusicStop();
void Scr_SoundFade();
int32_t Scr_PrecacheModel();
void Scr_PrecacheShellShock();
void Scr_PrecacheItem();
int32_t Scr_PrecacheShader();
void Scr_PrecacheString();
void Scr_AmbientPlay();
void Scr_AmbientStop();
void Scr_GrenadeExplosionEffect();
void GScr_RadiusDamage();
void GScr_RadiusDamageInternal(gentity_s *inflictor);
void GScr_EntityRadiusDamage(scr_entref_t entref);
void GScr_Detonate(scr_entref_t entref);
VariableUnion GScr_SetPlayerIgnoreRadiusDamage();
void GScr_DamageConeTrace(scr_entref_t entref);
void GScr_DamageConeTraceInternal(scr_entref_t entref, int32_t contentMask);
void GScr_SightConeTrace(scr_entref_t entref);
void GScr_GetMoveDelta();
void GScr_GetAngleDelta();
void GScr_GetNorthYaw();
void Scr_LoadFX();
void Scr_PlayFX();
void Scr_SetFxAngles(uint32_t givenAxisCount, float (*axis)[3], float *angles);
void Scr_FxParamError(uint32_t paramIndex, const char *errorString, int32_t fxId);
void Scr_PlayFXOnTag();
void Scr_PlayLoopedFX();
void Scr_SpawnFX();
void Scr_TriggerFX();
void Scr_PhysicsExplosionSphere();
void Scr_PhysicsRadiusJolt();
void Scr_PhysicsRadiusJitter();
void Scr_PhysicsExplosionCylinder();
void Scr_SetExponentialFog();
void Scr_SetFog(const char *cmd, float start, float density, float r, float g, float b, float time);
void Scr_VisionSetNaked();
void Scr_VisionSetNight();
void Scr_TableLookup();
void Scr_TableLookupIString();
void GScr_IsPlayer();
void GScr_IsPlayerNumber();
void GScr_SetWinningPlayer();
void GScr_SetWinningTeam();
void GScr_Announcement();
void GScr_ClientAnnouncement();
void GScr_GetTeamScore();
void GScr_SetTeamScore();
void GScr_SetClientNameMode();
void GScr_UpdateClientNames();
void GScr_GetTeamPlayersAlive();
void GScr_GetNumParts();
void GScr_GetPartName();
gentity_s *GScr_Earthquake();
void GScr_ShellShock(scr_entref_t entref);
void GScr_StopShellShock(scr_entref_t entref);
void GScr_GetTagOrigin(scr_entref_t entref);
int32_t GScr_UpdateTagInternal(
    gentity_s *ent,
    uint32_t tagName,
    cached_tag_mat_t *cachedTag,
    int32_t showScriptError);
void GScr_GetTagAngles(scr_entref_t entref);
void GScr_SetDepthOfField(scr_entref_t entref);
void GScr_SetViewModelDepthOfField(scr_entref_t entref);
void GScr_ViewKick(scr_entref_t entref);
void GScr_LocalToWorldCoords(scr_entref_t entref);
void GScr_SetRightArc(scr_entref_t entref);
void GScr_SetLeftArc(scr_entref_t entref);
void GScr_SetTopArc(scr_entref_t entref);
void GScr_SetBottomArc(scr_entref_t entref);
void GScr_PlaceSpawnPoint(scr_entref_t entref);
void GScr_MapRestart();
void GScr_LoadMap();
void GScr_ExitLevel();
void GScr_AddTestClient();
void GScr_MakeDvarServerInfo();
void GScr_AllClientsPrint();
void GScr_MapExists();
void GScr_IsValidGameType();
void GScr_SetVoteString();
void GScr_SetVoteTime();
void GScr_SetVoteYesCount();
void GScr_SetVoteNoCount();
void GScr_KickPlayer();
void GScr_BanPlayer();
void GScr_ClientPrint();
void GScr_OpenFile();
void GScr_CloseFile();
void GScr_FPrintln();
void Scr_FPrint_internal(bool commaBetweenFields);
void GScr_FPrintFields();
void GScr_FReadLn();
void GScr_FGetArg();
void GScr_GetStat(scr_entref_t entref);
void GScr_SetStat(scr_entref_t entref);
void GScr_SetTeamForTrigger(scr_entref_t entref);
void GScr_ClientClaimTrigger(scr_entref_t entref);
void GScr_ClientReleaseTrigger(scr_entref_t entref);
void GScr_ReleaseClaimedTrigger(scr_entref_t entref);
void GScr_SetMapCenter();
void GScr_SetGameEndTime();
void GScr_SetMiniMap();
bool GScr_SetTeamRadar();
void GScr_GetTeamRadar();
void GScr_GetArrayKeys();
void GScr_IsOnLadder(scr_entref_t entref);
void GScr_IsMantling(scr_entref_t entref);
void GScr_StartRagdoll(scr_entref_t entref);
void GScr_IsRagdoll(scr_entref_t entref);
void GScr_GetCorpseAnim(scr_entref_t entref);
void ScrCmd_ItemWeaponSetAmmo(scr_entref_t entref);
void Scr_SetOrigin(gentity_s *ent, int32_t i);
void Scr_SetAngles(gentity_s *ent, int32_t i);
void Scr_SetHealth(gentity_s *ent, int32_t i);
void GScr_AddEntity(gentity_s *pEnt);
void Scr_ParseGameTypeList();
XAssetHeader Scr_ParseGameTypeList_FastFile();
char *Scr_GetGameTypeNameForScript(const char *pszGameTypeScript);
bool Scr_IsValidGameType(const char *pszGameType);
void Scr_LoadGameType();
void Scr_StartupGameType();
void Scr_PlayerConnect(gentity_s *self);
void Scr_PlayerDisconnect(gentity_s *self);
void Scr_PlayerDamage(
    gentity_s *self,
    gentity_s *inflictor,
    gentity_s *attacker,
    int32_t damage,
    int32_t dflags,
    uint32_t meansOfDeath,
    uint32_t iWeapon,
    const float *vPoint,
    const float *vDir,
    hitLocation_t hitLoc,
    int32_t timeOffset);
void Scr_PlayerKilled(
    gentity_s *self,
    gentity_s *inflictor,
    gentity_s *attacker,
    int32_t damage,
    uint32_t meansOfDeath,
    uint32_t iWeapon,
    const float *vDir,
    hitLocation_t hitLoc,
    int32_t psTimeOffset,
    int32_t deathAnimDuration);
void Scr_PlayerLastStand(
    gentity_s *self,
    gentity_s *inflictor,
    gentity_s *attacker,
    int32_t damage,
    uint32_t meansOfDeath,
    uint32_t iWeapon,
    const float *vDir,
    hitLocation_t hitLoc,
    int32_t psTimeOffset);
void Scr_VoteCalled(gentity_s *self, char *command, char *param1, char *param2);
void Scr_PlayerVote(gentity_s *self, char *option);





// g_spawn_mp
struct SpawnFuncEntry // sizeof=0x8
{                                       // ...
    const char *classname;
    void(*callback)(gentity_s *);
};
void Scr_GetObjectField(uint32_t classnum, int entnum, int offset);
int32_t G_LevelSpawnString(const char *key, const char *defaultString, const char **out);
int32_t G_SpawnFloat(const char *key, const char *defaultString, float *out);
int32_t G_SpawnInt(const char *key, const char *defaultString, int32_t *out);
void Scr_ReadOnlyField(gentity_s *ent, int32_t i);
int32_t G_CallSpawnEntity(gentity_s *ent);
const gitem_s *G_GetItemForClassname(const char *classname, uint8_t model);
void(*G_FindSpawnFunc(
    const char *classname,
    const SpawnFuncEntry *spawnFuncArray,
    int32_t spawnFuncCount))(gentity_s *);
void GScr_AddFieldsForEntity();
void GScr_AddFieldsForRadiant();
void Scr_SetGenericField(uint8_t *b, fieldtype_t type, int32_t ofs);
int32_t Scr_SetObjectField(uint32_t classnum, uint32_t entnum, uint32_t offset);
int32_t Scr_SetEntityField(uint32_t entnum, uint32_t offset);
void Scr_GetEntityField(uint32_t entnum, uint32_t offset);
void Scr_GetGenericField(uint8_t *b, fieldtype_t type, int32_t ofs);
void Scr_FreeEntityConstStrings(gentity_s *pEnt);
void Scr_FreeEntity(gentity_s *ent);
void Scr_AddEntity(gentity_s *ent);
gentity_s *Scr_GetEntityAllowNull(uint32_t index);
gentity_s *Scr_GetEntity(uint32_t index);
void Scr_FreeHudElem(game_hudelem_s *hud);
void Scr_AddHudElem(game_hudelem_s *hud);
uint16_t Scr_ExecEntThread(gentity_s *ent, int32_t handle, uint32_t paramcount);
void Scr_Notify(gentity_s *ent, uint16_t stringValue, uint32_t paramcount);
void Scr_GetEnt();
void Scr_GetEntArray();
void SP_worldspawn();
void G_SpawnEntitiesFromString();
void G_CallSpawn();
void G_ParseEntityFields(gentity_s *ent);
void G_ParseEntityField(const char *key, char *value, gentity_s *ent);
void G_SetEntityScriptVariable(const char *key, char *value, gentity_s *ent);
uint32_t G_SetEntityScriptVariableInternal(const char *key, char *value);
void G_LoadStructs();
int32_t G_SpawnStruct();




// g_trigger_mp
void G_Trigger(gentity_s *self, gentity_s *other);
char InitTrigger(gentity_s *self);
void InitSentientTrigger(gentity_s *self);
void multi_trigger(gentity_s *ent);
void Touch_Multi(gentity_s *self, gentity_s *other, int32_t extra);
void SP_trigger_multiple(gentity_s *ent);
void InitTriggerWait(gentity_s *ent, int32_t spawnflag);
void SP_trigger_radius(gentity_s *ent);
void SP_trigger_disk(gentity_s *ent);
void hurt_touch(gentity_s *self, gentity_s *other, int32_t extra);
void hurt_use(gentity_s *self, gentity_s *other, gentity_s *third);
void SP_trigger_hurt(gentity_s *self);
void SP_trigger_once(gentity_s *ent);
bool Respond_trigger_damage(gentity_s *pEnt, int32_t iMOD);
void Activate_trigger_damage(gentity_s *pEnt, gentity_s *pOther, int32_t iDamage, int32_t iMOD);
void Use_trigger_damage(gentity_s *pEnt, gentity_s *pOther, gentity_s *third);
void Pain_trigger_damage(gentity_s *pSelf, gentity_s *pAttacker, int32_t iDamage, const float *vPoint, int32_t iMod, const float *idk, hitLocation_t hit, int32_t swag);
void Die_trigger_damage(
    gentity_s *pSelf,
    gentity_s *pInflictor,
    gentity_s *pAttacker,
    int32_t iDamage,
    int32_t iMod,
    int32_t iWeapon,
    const float *vDir,
    const hitLocation_t hitLoc,
    int32_t timeOffset);
void SP_trigger_damage(gentity_s *pSelf);
void G_CheckHitTriggerDamage(gentity_s *pActivator, float *vStart, float *vEnd, int32_t iDamage, uint32_t iMOD);
void G_GrenadeTouchTriggerDamage(gentity_s *pActivator, float *vStart, float *vEnd, int32_t iDamage, int32_t iMOD);
void SP_trigger_lookat(gentity_s *self);



// g_vehicles_mp
uint16_t BG_VehiclesGetSlotTagName(int32_t slotIndex);
void CG_VehRegisterDvars();
DObj_s *GetVehicleEntDObj(int32_t localClientNum, centity_s *centVeh);
void CG_VehGunnerPOV(int32_t localClientNum, float *resultOrigin, float *resultAngles);
clientInfo_t *ClientInfoForLocalClient(int32_t localClientNum);
bool CG_VehLocalClientUsingVehicle(int32_t localClientNum);
bool CG_VehLocalClientDriving(int32_t localClientNum);
bool CG_VehEntityUsingVehicle(int32_t localClientNum, uint32_t entNum);
clientInfo_t *ClientInfoForEntity(int32_t localClientNum, uint32_t entNum);
int32_t CG_VehLocalClientVehicleSlot(int32_t localClientNum);
int32_t CG_VehPlayerVehicleSlot(int32_t localClientNum, uint32_t entNum);
void CG_VehSeatTransformForPlayer(
    int32_t localClientNum,
    uint32_t entNum,
    float *resultOrigin,
    float *resultAngles);
void SeatTransformForClientInfo(int32_t localClientNum, clientInfo_t *ci, float *resultOrigin, float *resultAngles);
void SeatTransformForSlot(
    int32_t localClientNum,
    uint32_t vehEntNum,
    uint32_t vehSlotIdx,
    float *resultOrigin,
    float *resultAngles);
void CG_VehSeatOriginForLocalClient(int32_t localClientNum, float *result);
double Veh_GetTurretBarrelRoll(int32_t localClientNum, centity_s *cent);
int32_t CG_GetEntityIndex(int32_t localClientNum, const centity_s *cent);
vehicleEffects *VehicleGetFxInfo(int32_t localClientNum, int32_t entityNum);
void Veh_IncTurretBarrelRoll(int32_t localClientNum, int32_t entityNum, float rotation);
void CG_VehProcessEntity(int32_t localClientNum, centity_s *cent);
void SetupPoseControllers(int32_t localClientNum, DObj_s *obj, centity_s *cent, vehfx_t *fxInfo);
uint16_t CompressUnit(float unit);
void VehicleFXTest(int32_t localClientNum, const DObj_s *obj, centity_s *cent, vehfx_t *fxInfo);
double GetSpeed(int32_t localClientNum, centity_s *cent);
void VEH_SetPosition(gentity_s *ent, const float *origin, const float *vel, const float *angles);
void VEH_InitPhysics(gentity_s *ent);
bool VEH_SlideMove(gentity_s *ent, int32_t gravity, float frameTime);
void VEH_ClipVelocity(float *in, float *normal, float *out);
void VEH_BackupPosition(gentity_s *ent);
void VEH_TouchEntities(gentity_s *ent);
void VEH_PushEntity(gentity_s *ent, gentity_s *target, float *pushDir, float *deltaOrigin, float *deltaAngles);
bool AttachedStickyMissile(gentity_s *vehicle, gentity_s *missile);
void PushAttachedStickyMissile(gentity_s *vehicle, gentity_s *missile);
void G_VehRegisterDvars();
struct vehicle_info_t *VEH_GetVehicleInfo(__int16 index);
int32_t G_VehPlayerRideSlot(gentity_s *vehicle, int32_t playerEntNum);
void VEH_DebugCapsule(float *pos, float rad, float height, float r, float g, float b);
void VEH_SetPosition(gentity_s *ent, const float *origin, const float *angles);
void VEH_JoltBody(gentity_s *ent, const float *dir, float intensity, float speedFrac, float decel);
void VEH_UpdateSounds(gentity_s *ent);
void G_VehUnlinkPlayer(gentity_s *ent, gentity_s *player);
void VehicleClearRideSlotForPlayer(gentity_s *ent, int32_t playerEntNum);
void G_VehiclesInit(int32_t restarting);
void InitInfos(int32_t restarting);
void G_VehiclesSetupSpawnedEnts();
void SetupCollisionMap(gentity_s *ent);
gentity_s *GetCollisionMap(const char *modelname);
void SpawnVehicle(gentity_s *ent, const char *typeName);
int32_t VEH_GetVehicleInfoFromName(const char *name);
int32_t G_LoadVehicle(const char *name);
int32_t VEH_ParseSpecificField(uint8_t *pStruct, const char *pValue, int32_t fieldType);
void VEH_InitModelAndValidateTags(gentity_s *ent, int32_t *infoIdx);
char VEH_DObjHasRequiredTags(gentity_s *ent, int32_t infoIdx);
void InitVehicleTags(gentity_s *ent);
void InitEntityVehicleVars(gentity_s *ent, scr_vehicle_s *veh, __int16 infoIdx);
void InitEntityVars(gentity_s *ent, scr_vehicle_s *veh, int32_t infoIdx);
void G_VehFreeEntity(gentity_s *vehEnt);
bool G_VehUsable(gentity_s *vehicle, gentity_s *player);
char VehicleHasSeatFree(gentity_s *ent);
bool G_VehImmuneToDamage(gentity_s *ent, int32_t mod, char damageFlags, uint32_t weapon);
void G_VehEntHandler_Think(gentity_s *pSelf);
VehicleTags *RideTagFirst(gentity_s *ent);
VehicleTags *RideTagNext(gentity_s *ent, VehicleRideSlot_t *inTag);
void VEH_DebugBox(float *pos, float width, float r, float g, float b);
void InflictDamage(gentity_s *vehEnt, gentity_s *target, float *dir, int32_t damage);
int32_t VehicleEntDriver(gentity_s *ent);
void UpdateTurret(gentity_s *ent);
int32_t VehicleEntGunner(gentity_s *ent);
void VEH_UpdateBody(gentity_s *ent, float frameTime);
void VEH_UpdateSteering(gentity_s *ent);
void VEH_UpdateMaterialTime(gentity_s *ent, float frameTime);
void VEH_UpdateWeapon(gentity_s *ent);
void FireTurret(gentity_s *ent, gentity_s *player);
void FillWeaponParms(gentity_s *vehEnt, gentity_s *player, weaponParms *wp);
void VEH_UpdateClients(gentity_s *ent);
void VEH_UpdateClientPassenger(gentity_s *ent);
int32_t VehicleEntPassenger(gentity_s *ent);
void VEH_UpdateClientGunner(gentity_s *ent);
void VEH_UpdateClientDriver(gentity_s *ent);
char VEH_PlayerRotation(gentity_s *player, vehicle_physic_t *phys);
void UpdateSimulation(gentity_s *ent);
void IntegratePosAndRot(gentity_s *ent);
void DebugDrawVelocity(const float *origin, const float *velocity, const float *color);
void GetAccelerationForces(gentity_s *ent, float frameTime, float *resultPosition);
void PositionAccelForces(vehicle_physic_t *phys, float driverAccel, float *result);
bool CarTravelingForward(vehicle_physic_t *phys);
bool DriverBreaking(vehicle_physic_t *phys, float driverAccel);
void AdvanceVehiclePosition(gentity_s *ent, float frameTime);
void VEH_GroundTrace(gentity_s *ent);
void VEH_GroundMove(gentity_s *ent, float frameTime);
void VEH_StepSlideMove(gentity_s *ent, int32_t gravity, float frameTime);
bool VEH_SlideMove(gentity_s *ent, int32_t gravity, float frameTime);
void VEH_AirMove(gentity_s *ent, int32_t gravity, float frameTime);
void VEH_GroundPlant(gentity_s *ent, int32_t gravity, float frameTime);
void VEH_DebugLine(float *start, float *end, float r, float g, float b);
void VEH_GetWheelOrigin(gentity_s *ent, int32_t idx, float *origin);
void AdvanceVehicleRotation(gentity_s *ent, float frameTime);
void CapMaxMPH(vehicle_physic_t *phys);
void InitFirstThink(gentity_s *pSelf);
void G_VehEntHandler_Touch(gentity_s *pSelf, gentity_s *pOther, int32_t bTouched);
void G_VehEntHandler_Use(gentity_s *pEnt, gentity_s *pOther, gentity_s *pActivator);
void LinkPlayerToVehicle(gentity_s *ent, gentity_s *player);
void G_VehEntHandler_Die(
    gentity_s *pSelf,
    gentity_s *pInflictor,
    gentity_s *pAttacker,
    const int32_t damage,
    const int32_t mod,
    const int32_t weapon,
    const float *dir,
    const hitLocation_t hitLoc,
    int32_t psTimeOffset);
void G_VehEntHandler_Controller(const gentity_s *pSelf, int32_t *partBits);
void G_VehSpawner(gentity_s *pSelf);
void G_VehCollmapSpawner(gentity_s *pSelf);




// player_use_mp
struct useList_t // sizeof=0x8
{                                       // ...
    gentity_s *ent;                     // ...
    float score;
};
static_assert(LAYOUT_32BIT(sizeof(useList_t) == 0x8));

void Player_UpdateActivate(gentity_s *ent);
char Player_ActivateCmd(gentity_s *ent);
void Player_ActivateHoldCmd(gentity_s *ent);
void Player_UseEntity(gentity_s *playerEnt, gentity_s *useEnt);
void Player_UpdateCursorHints(gentity_s *ent);
int32_t Player_GetUseList(gentity_s *ent, useList_t *useList, int32_t prevHintEntIndex);
int32_t compare_use(float *pe1, float *pe2);
int32_t Player_GetItemCursorHint(const gclient_s *client, const gentity_s *traceEnt);
void Player_SetTurretDropHint(gentity_s *ent);
void Player_SetVehicleDropHint(gentity_s *ent);



extern const dvar_t *vehHelicopterTiltFromControllerAxes;
extern const dvar_t *vehHelicopterTiltFromFwdAndYaw;
extern const dvar_t *vehHelicopterJitterJerkyness;
extern const dvar_t *vehHelicopterTiltSpeed;
extern const dvar_t *vehHelicopterInvertUpDown;
extern const dvar_t *vehHelicopterMaxYawAccel;
extern const dvar_t *vehHelicopterLookaheadTime;
extern const dvar_t *vehHelicopterMaxSpeedVertical;
extern const dvar_t *vehHelicopterTiltFromDeceleration;
extern const dvar_t *vehHelicopterRightStickDeadzone;
extern const dvar_t *vehHelicopterSoftCollisions;
extern const dvar_t *vehHelicopterYawOnLeftStick;
extern const dvar_t *vehHelicopterDecelerationFwd;
extern const dvar_t *vehHelicopterMaxAccelVertical;
extern const dvar_t *vehHelicopterMaxPitch;
extern const dvar_t *vehHelicopterTiltFromAcceleration;
extern const dvar_t *vehHelicopterStrafeDeadzone;
extern const dvar_t *vehHelicopterHoverSpeedThreshold;
extern const dvar_t *vehHelicopterMaxYawRate;
extern const dvar_t *vehHelicopterMaxAccel;
extern const dvar_t *vehHelicopterYawAltitudeControls;
extern const dvar_t *vehHelicopterMaxRoll;
extern const dvar_t *vehHelicopterScaleMovement;
extern const dvar_t *vehHelicopterMaxSpeed;
extern const dvar_t *vehHelicopterTiltFromVelocity;
extern const dvar_t *vehHelicopterTiltMomentum;
extern const dvar_t *vehHelicopterHeadSwayDontSwayTheTurret;
extern const dvar_t *vehHelicopterTiltFromFwdAndYaw_VelAtMaxTilt;
extern const dvar_t *vehHelicopterDecelerationSide;

void Scr_AddStruct();
void Scr_ResetTimeout();

extern BuiltinFunctionDef functions[];
extern BuiltinMethodDef methods_2[];
extern uint16_t *controller_names[6];


const entityHandler_t entityHandlers[] =
{
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
  { NULL, NULL, NULL, &Touch_Multi, NULL, NULL, NULL, NULL, 0, 0 },
  { NULL, NULL, NULL, NULL, &hurt_use, NULL, NULL, NULL, 0, 0 },
  { NULL, NULL, NULL, &hurt_touch, &hurt_use, NULL, NULL, NULL, 0, 0 },
  {
    NULL,
    NULL,
    NULL,
    NULL,
    &Use_trigger_damage,
    &Pain_trigger_damage,
    &Die_trigger_damage,
    NULL,
    0,
    0
  },
  { NULL, &Reached_ScriptMover, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
  { NULL, &Reached_ScriptMover, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
  {
    &G_ExplodeMissile,
    NULL,
    NULL,
    &Touch_Item_Auto,
    NULL,
    NULL,
    NULL,
    NULL,
    3,
    4
  },
  { &G_TimedObjectThink, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
  { &G_ExplodeMissile, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 5, 6 },
  { NULL, NULL, NULL, NULL, NULL, NULL, &player_die, &G_PlayerController, 0, 0 },
  { NULL, NULL, NULL, NULL, NULL, NULL, &player_die, NULL, 0, 0 },
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL, &G_PlayerController, 0, 0 },
  { &BodyEnd, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
  {
    &turret_think_init,
    NULL,
    NULL,
    NULL,
    &turret_use,
    NULL,
    NULL,
    &turret_controller,
    0,
    0
  },
  {
    &turret_think,
    NULL,
    NULL,
    NULL,
    &turret_use,
    NULL,
    NULL,
    &turret_controller,
    0,
    0
  },
  {
    &DroppedItemClearOwner,
    NULL,
    NULL,
    &Touch_Item_Auto,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0
  },
  {
    &FinishSpawningItem,
    NULL,
    NULL,
    &Touch_Item_Auto,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0
  },
  { NULL, NULL, NULL, &Touch_Item_Auto, NULL, NULL, NULL, NULL, 0, 0 },
    //{ NULL, NULL, NULL, NULL, &KISAK_NULLSUB, NULL, NULL, NULL, 0, 0 },
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
    { NULL, &Reached_ScriptMover, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
    { &G_FreeEntity, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0 },
    {
      &G_VehEntHandler_Think,
      NULL,
      NULL,
      &G_VehEntHandler_Touch,
      &G_VehEntHandler_Use,
      &Helicopter_Pain,
      &G_VehEntHandler_Die,
      &G_VehEntHandler_Controller,
      0,
      0
    },
    {
      &Helicopter_Think,
      NULL,
      NULL,
      NULL,
      NULL,
      &Helicopter_Pain,
      &Helicopter_Die,
      &Helicopter_Controller,
      0,
      0
    }
}; // idb
