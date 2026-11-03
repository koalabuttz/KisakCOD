#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct gentity_s;
struct sentient_s;
struct actor_s;
struct ScriptFunctions;
struct weaponParms;
struct TurretInfo;

void TRACK_turret();
void G_InitTurrets();
int Turret_GetRemainingConvergenceTime(const TurretInfo *turretInfo, unsigned int type);
void Turret_FillWeaponParms(const gentity_s *ent, const gentity_s *activator, weaponParms *wp);
bool MayHitTarget(const weaponParms *weapon, const gentity_s *target, const float *forward);
void Fire_Lead(gentity_s *ent, gentity_s *activator, int bUseAccuracy);
void clamp_playerbehindgun(gentity_s *self, gentity_s *other);
void turret_clientaim(gentity_s *self, gentity_s *other);
void turret_shoot_internal(gentity_s *self, gentity_s *other);
void turret_track(gentity_s *self, gentity_s *other);
void turret_UpdateSound(gentity_s *self);
void turret_CalculateConvergenceAngularVelocity(const gentity_s *self, const float *desiredAngles, float *angularVelocity);
int turret_UpdateTargetAngles(gentity_s *self, const float *desiredAngles, int bManned);
bool turret_IsFiringInternal(int state);
bool turret_IsFiring(gentity_s *self);
void turret_SetState(gentity_s *self, unsigned int state);
void turret_ClearTargetEnt(gentity_s *self);
int turret_ReturnToDefaultPos(gentity_s *self, int bManned);
void turret_SetDefaultDropPitch(gentity_s *self, double pitch);
void turret_shoot(gentity_s *self);
int turret_CanTargetPoint(const gentity_s *self, const float *vPoint, float *vSource, float *localAngles);
int turret_CanTargetSentient(
    const gentity_s *self,
    const sentient_s *sentient,
    float *targetPosition,
    float *muzzlePosition,
    float *localAngles);
void turret_aimat_vector_internal(gentity_s *self, float *origin, int bShoot, const float *desiredAngles);
int turret_aimat_vector(gentity_s *self, float *origin, int bShoot, float *desiredAngles);
void turret_SetTargetEnt(gentity_s *self, gentity_s *ent);
int turret_aimat_Sentient_Internal(
    gentity_s *self,
    sentient_s *enemy,
    int bShoot,
    int missTime,
    float *desiredAngles);
int turret_aimat_Sentient(gentity_s *self, sentient_s *enemy, int bShoot, int missTime);
int turret_aimat_Ent(gentity_s *self, gentity_s *ent, int bShoot);
bool turret_SightTrace(const float *start, const float *end, int passEnt1, int passEnt2);
int turret_isTargetTooCloseToPlayer(
    const float *flashOrigin,
    const gentity_s *turret,
    const gentity_s *target);
int turret_isTargetVisible(gentity_s *self, const gentity_s *target, float *distSqr);
sentient_s *turret_findBestTarget(gentity_s *self);
void turret_think_auto_nonai(gentity_s *self);
int turret_think_auto(gentity_s *self, actor_s *actor);
int turret_think_manual(gentity_s *self, actor_s *actor);
void turret_RestoreDefaultDropPitch(gentity_s *self);
void turret_think(gentity_s *self);
bool Actor_IsTurretCloserThenCurrent(actor_s *actor, gentity_s *turret);
bool turret_canuse_auto(gentity_s *self, actor_s *actor);
bool turret_canuse_manual(gentity_s *self, actor_s *actor);
bool turret_canuse(actor_s *actor, gentity_s *pTurret);
void turret_controller(const gentity_s *self, int *partBits);
void SP_turret_XAnimPrecache(ScriptFunctions *functions, const char *classname);
bool turret_behind(gentity_s *self, gentity_s *other);
bool G_IsTurretUsable(gentity_s *self, gentity_s *owner);
void G_DeactivateTurret(gentity_s *self);
void turret_use(gentity_s *self, gentity_s *owner, gentity_s *activator);
int G_CanSpawnTurret();
void G_SpawnTurret(gentity_s *self, const char *weaponinfoname);
void SP_turret(gentity_s *self);
void G_ClientStopUsingTurret(gentity_s *self);
void turret_think_client(gentity_s *self);
void turret_think_init(gentity_s *self);
void G_FreeTurret(gentity_s *self);
