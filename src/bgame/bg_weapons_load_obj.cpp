#include <universal/q_shared.h>
#include "bg_local.h"
#include "bg_public.h"
#include <qcommon/mem_track.h>
#include <database/database.h>
#include <universal/q_parse.h>
#include <universal/com_memory.h>
#include <universal/com_files.h>
#include <universal/com_sndalias.h>
#include <universal/surfaceflags.h>

//int surfaceTypeSoundListCount 828010f0     bg_weapons_load_obj.obj
//struct SurfaceTypeSoundList *surfaceTypeSoundLists 828011f8     bg_weapons_load_obj.obj

uint32_t g_playerAnimTypeNamesCount;

SurfaceTypeSoundList surfaceTypeSoundLists[16];

const char *stickinessNames[4] =
{
  "Don't stick",
  "Stick to all",
  "Stick to ground",
  "Stick to ground, maintain yaw"
}; // idb
const char *weapIconRatioNames[3] = { "1:1", "2:1", "4:1" }; // idb
const char *ammoCounterClipNames[7] =
{
  "None",
  "Magazine",
  "ShortMagazine",
  "Shotgun",
  "Rocket",
  "Beltfed",
  "AltWeapon"
}; // idb
const char *overlayInterfaceNames[3] = { "None", "Javelin", "Turret Scope" }; // idb
const char *szWeapFireTypeNames[5] =
{
  "Full Auto",
  "Single Shot",
  "2-Round Burst",
  "3-Round Burst",
  "4-Round Burst"
}; // idb
const char *szWeapInventoryTypeNames[4] = { "primary", "offhand", "item", "altmode" }; // idb
const char *penetrateTypeNames[4] = { "none", "small", "medium", "large" }; // idb
const char *szWeapOverlayReticleNames[2] = { "none", "crosshair" }; // idb
const char *szWeapStanceNames[3] = { "stand", "duck", "prone" }; // idb
const char *accuracyDirName[3] = { "aivsai", "aivsplayer", NULL }; // idb
const char *activeReticleNames[3] = { "None", "Pip-On-A-Stick", "Bouncing diamond" }; // idb
const char *szWeapTypeNames[4] = { "bullet", "grenade", "projectile", "binoculars" }; // idb
const char *guidedMissileNames[4] = { "None", "Sidewinder", "Hellfire", "Javelin" }; // idb
const char *offhandClassNames[4] = { "None", "Frag Grenade", "Smoke Grenade", "Flash Grenade" }; // idb
const char *szProjectileExplosionNames[7] = { "grenade", "rocket", "flashbang", "none", "dud", "smoke", "heavy explosive" }; // idb

const char *impactTypeNames[9] =
{
  "none",
  "bullet_small",
  "bullet_large",
  "bullet_ap",
  "shotgun",
  "grenade_bounce",
  "grenade_explode",
  "rocket_explode",
  "projectile_dud"
}; // idb

// The field offsets were decompiled as 32-bit numbers. A 64-bit build lays
// WeaponDef out differently (its pointers are 8 bytes), so a client with
// fs_game set, which parses weapon files with this table, wrote every field to
// the wrong place. Each entry now names its member; the 32-bit number stays
// beside it and must still match on 32-bit builds.
template <size_t Offset, size_t Offset32>
struct WeaponFieldOffset
{
    static_assert(sizeof(void *) != 4 || Offset == Offset32, "weapon field moved on a 32-bit build");
    static constexpr int value = static_cast<int>(Offset);
};
#define WEAPON_FIELD(member, offset32) (WeaponFieldOffset<offsetof(WeaponDef, member), offset32>::value)

cspField_t weaponDefFields[502] =
{
  { "displayName", WEAPON_FIELD(szDisplayName, 4), CSPFT_STRING },
  { "AIOverlayDescription", WEAPON_FIELD(szOverlayName, 8), CSPFT_STRING },
  { "modeName", WEAPON_FIELD(szModeName, 212), CSPFT_STRING },
  { "playerAnimType", WEAPON_FIELD(playerAnimType, 296), WFT_ANIMTYPE },
  { "gunModel", WEAPON_FIELD(gunXModel[0], 12), CSPFT_XMODEL },
  { "gunModel2", WEAPON_FIELD(gunXModel[1], 16), CSPFT_XMODEL },
  { "gunModel3", WEAPON_FIELD(gunXModel[2], 20), CSPFT_XMODEL },
  { "gunModel4", WEAPON_FIELD(gunXModel[3], 24), CSPFT_XMODEL },
  { "gunModel5", WEAPON_FIELD(gunXModel[4], 28), CSPFT_XMODEL },
  { "gunModel6", WEAPON_FIELD(gunXModel[5], 32), CSPFT_XMODEL },
  { "gunModel7", WEAPON_FIELD(gunXModel[6], 36), CSPFT_XMODEL },
  { "gunModel8", WEAPON_FIELD(gunXModel[7], 40), CSPFT_XMODEL },
  { "gunModel9", WEAPON_FIELD(gunXModel[8], 44), CSPFT_XMODEL },
  { "gunModel10", WEAPON_FIELD(gunXModel[9], 48), CSPFT_XMODEL },
  { "gunModel11", WEAPON_FIELD(gunXModel[10], 52), CSPFT_XMODEL },
  { "gunModel12", WEAPON_FIELD(gunXModel[11], 56), CSPFT_XMODEL },
  { "gunModel13", WEAPON_FIELD(gunXModel[12], 60), CSPFT_XMODEL },
  { "gunModel14", WEAPON_FIELD(gunXModel[13], 64), CSPFT_XMODEL },
  { "gunModel15", WEAPON_FIELD(gunXModel[14], 68), CSPFT_XMODEL },
  { "gunModel16", WEAPON_FIELD(gunXModel[15], 72), CSPFT_XMODEL },
  { "handModel", WEAPON_FIELD(handXModel, 76), CSPFT_XMODEL },
  { "hideTags", WEAPON_FIELD(hideTags[0], 216), WFT_HIDETAGS },
  { "notetrackSoundMap", WEAPON_FIELD(notetrackSoundMapKeys[0], 232), WFT_NOTETRACKSOUNDMAP },
  { "idleAnim", WEAPON_FIELD(szXAnims[1], 84), CSPFT_STRING },
  { "emptyIdleAnim", WEAPON_FIELD(szXAnims[2], 88), CSPFT_STRING },
  { "fireAnim", WEAPON_FIELD(szXAnims[3], 92), CSPFT_STRING },
  { "holdFireAnim", WEAPON_FIELD(szXAnims[4], 96), CSPFT_STRING },
  { "lastShotAnim", WEAPON_FIELD(szXAnims[5], 100), CSPFT_STRING },
  { "detonateAnim", WEAPON_FIELD(szXAnims[25], 180), CSPFT_STRING },
  { "rechamberAnim", WEAPON_FIELD(szXAnims[6], 104), CSPFT_STRING },
  { "meleeAnim", WEAPON_FIELD(szXAnims[7], 108), CSPFT_STRING },
  { "meleeChargeAnim", WEAPON_FIELD(szXAnims[8], 112), CSPFT_STRING },
  { "reloadAnim", WEAPON_FIELD(szXAnims[9], 116), CSPFT_STRING },
  { "reloadEmptyAnim", WEAPON_FIELD(szXAnims[10], 120), CSPFT_STRING },
  { "reloadStartAnim", WEAPON_FIELD(szXAnims[11], 124), CSPFT_STRING },
  { "reloadEndAnim", WEAPON_FIELD(szXAnims[12], 128), CSPFT_STRING },
  { "raiseAnim", WEAPON_FIELD(szXAnims[13], 132), CSPFT_STRING },
  { "dropAnim", WEAPON_FIELD(szXAnims[15], 140), CSPFT_STRING },
  { "firstRaiseAnim", WEAPON_FIELD(szXAnims[14], 136), CSPFT_STRING },
  { "altRaiseAnim", WEAPON_FIELD(szXAnims[16], 144), CSPFT_STRING },
  { "altDropAnim", WEAPON_FIELD(szXAnims[17], 148), CSPFT_STRING },
  { "quickRaiseAnim", WEAPON_FIELD(szXAnims[18], 152), CSPFT_STRING },
  { "quickDropAnim", WEAPON_FIELD(szXAnims[19], 156), CSPFT_STRING },
  { "emptyRaiseAnim", WEAPON_FIELD(szXAnims[20], 160), CSPFT_STRING },
  { "emptyDropAnim", WEAPON_FIELD(szXAnims[21], 164), CSPFT_STRING },
  { "sprintInAnim", WEAPON_FIELD(szXAnims[22], 168), CSPFT_STRING },
  { "sprintLoopAnim", WEAPON_FIELD(szXAnims[23], 172), CSPFT_STRING },
  { "sprintOutAnim", WEAPON_FIELD(szXAnims[24], 176), CSPFT_STRING },
  { "nightVisionWearAnim", WEAPON_FIELD(szXAnims[26], 184), CSPFT_STRING },
  { "nightVisionRemoveAnim", WEAPON_FIELD(szXAnims[27], 188), CSPFT_STRING },
  { "adsFireAnim", WEAPON_FIELD(szXAnims[28], 192), CSPFT_STRING },
  { "adsLastShotAnim", WEAPON_FIELD(szXAnims[29], 196), CSPFT_STRING },
  { "adsRechamberAnim", WEAPON_FIELD(szXAnims[30], 200), CSPFT_STRING },
  { "adsUpAnim", WEAPON_FIELD(szXAnims[31], 204), CSPFT_STRING },
  { "adsDownAnim", WEAPON_FIELD(szXAnims[32], 208), CSPFT_STRING },
  { "script", WEAPON_FIELD(szScript, 2036), CSPFT_STRING },
  { "weaponType", WEAPON_FIELD(weapType, 300), WFT_WEAPONTYPE },
  { "weaponClass", WEAPON_FIELD(weapClass, 304), WFT_WEAPONCLASS },
  { "penetrateType", WEAPON_FIELD(penetrateType, 308), WFT_PENETRATE_TYPE },
  { "impactType", WEAPON_FIELD(impactType, 312), WFT_IMPACT_TYPE },
  { "inventoryType", WEAPON_FIELD(inventoryType, 316), WFT_INVENTORYTYPE },
  { "fireType", WEAPON_FIELD(fireType, 320), WFT_FIRETYPE },
  { "offhandClass", WEAPON_FIELD(offhandClass, 324), WFT_OFFHAND_CLASS },
  { "viewFlashEffect", WEAPON_FIELD(viewFlashEffect, 332), CSPFT_FX },
  { "worldFlashEffect", WEAPON_FIELD(worldFlashEffect, 336), CSPFT_FX },
  { "pickupSound", WEAPON_FIELD(pickupSound, 340), CSPFT_SOUND },
  { "pickupSoundPlayer", WEAPON_FIELD(pickupSoundPlayer, 344), CSPFT_SOUND },
  { "ammoPickupSound", WEAPON_FIELD(ammoPickupSound, 348), CSPFT_SOUND },
  { "ammoPickupSoundPlayer", WEAPON_FIELD(ammoPickupSoundPlayer, 352), CSPFT_SOUND },
  { "projectileSound", WEAPON_FIELD(projectileSound, 356), CSPFT_SOUND },
  { "pullbackSound", WEAPON_FIELD(pullbackSound, 360), CSPFT_SOUND },
  { "pullbackSoundPlayer", WEAPON_FIELD(pullbackSoundPlayer, 364), CSPFT_SOUND },
  { "fireSound", WEAPON_FIELD(fireSound, 368), CSPFT_SOUND },
  { "fireSoundPlayer", WEAPON_FIELD(fireSoundPlayer, 372), CSPFT_SOUND },
  { "loopFireSound", WEAPON_FIELD(fireLoopSound, 376), CSPFT_SOUND },
  { "loopFireSoundPlayer", WEAPON_FIELD(fireLoopSoundPlayer, 380), CSPFT_SOUND },
  { "stopFireSound", WEAPON_FIELD(fireStopSound, 384), CSPFT_SOUND },
  { "stopFireSoundPlayer", WEAPON_FIELD(fireStopSoundPlayer, 388), CSPFT_SOUND },
  { "lastShotSound", WEAPON_FIELD(fireLastSound, 392), CSPFT_SOUND },
  { "lastShotSoundPlayer", WEAPON_FIELD(fireLastSoundPlayer, 396), CSPFT_SOUND },
  { "emptyFireSound", WEAPON_FIELD(emptyFireSound, 400), CSPFT_SOUND },
  { "emptyFireSoundPlayer", WEAPON_FIELD(emptyFireSoundPlayer, 404), CSPFT_SOUND },
  { "meleeSwipeSound", WEAPON_FIELD(meleeSwipeSound, 408), CSPFT_SOUND },
  { "meleeSwipeSoundPlayer", WEAPON_FIELD(meleeSwipeSoundPlayer, 412), CSPFT_SOUND },
  { "meleeHitSound", WEAPON_FIELD(meleeHitSound, 416), CSPFT_SOUND },
  { "meleeMissSound", WEAPON_FIELD(meleeMissSound, 420), CSPFT_SOUND },
  { "rechamberSound", WEAPON_FIELD(rechamberSound, 424), CSPFT_SOUND },
  { "rechamberSoundPlayer", WEAPON_FIELD(rechamberSoundPlayer, 428), CSPFT_SOUND },
  { "reloadSound", WEAPON_FIELD(reloadSound, 432), CSPFT_SOUND },
  { "reloadSoundPlayer", WEAPON_FIELD(reloadSoundPlayer, 436), CSPFT_SOUND },
  { "reloadEmptySound", WEAPON_FIELD(reloadEmptySound, 440), CSPFT_SOUND },
  { "reloadEmptySoundPlayer", WEAPON_FIELD(reloadEmptySoundPlayer, 444), CSPFT_SOUND },
  { "reloadStartSound", WEAPON_FIELD(reloadStartSound, 448), CSPFT_SOUND },
  { "reloadStartSoundPlayer", WEAPON_FIELD(reloadStartSoundPlayer, 452), CSPFT_SOUND },
  { "reloadEndSound", WEAPON_FIELD(reloadEndSound, 456), CSPFT_SOUND },
  { "reloadEndSoundPlayer", WEAPON_FIELD(reloadEndSoundPlayer, 460), CSPFT_SOUND },
  { "detonateSound", WEAPON_FIELD(detonateSound, 464), CSPFT_SOUND },
  { "detonateSoundPlayer", WEAPON_FIELD(detonateSoundPlayer, 468), CSPFT_SOUND },
  { "nightVisionWearSound", WEAPON_FIELD(nightVisionWearSound, 472), CSPFT_SOUND },
  { "nightVisionWearSoundPlayer", WEAPON_FIELD(nightVisionWearSoundPlayer, 476), CSPFT_SOUND },
  { "nightVisionRemoveSound", WEAPON_FIELD(nightVisionRemoveSound, 480), CSPFT_SOUND },
  { "nightVisionRemoveSoundPlayer", WEAPON_FIELD(nightVisionRemoveSoundPlayer, 484), CSPFT_SOUND },
  { "raiseSound", WEAPON_FIELD(raiseSound, 496), CSPFT_SOUND },
  { "raiseSoundPlayer", WEAPON_FIELD(raiseSoundPlayer, 500), CSPFT_SOUND },
  { "firstRaiseSound", WEAPON_FIELD(firstRaiseSound, 504), CSPFT_SOUND },
  { "firstRaiseSoundPlayer", WEAPON_FIELD(firstRaiseSoundPlayer, 508), CSPFT_SOUND },
  { "altSwitchSound", WEAPON_FIELD(altSwitchSound, 488), CSPFT_SOUND },
  { "altSwitchSoundPlayer", WEAPON_FIELD(altSwitchSoundPlayer, 492), CSPFT_SOUND },
  { "putawaySound", WEAPON_FIELD(putawaySound, 512), CSPFT_SOUND },
  { "putawaySoundPlayer", WEAPON_FIELD(putawaySoundPlayer, 516), CSPFT_SOUND },
  { "bounceSound", WEAPON_FIELD(bounceSound, 520), WFT_BOUNCE_SOUND },
  { "viewShellEjectEffect", WEAPON_FIELD(viewShellEjectEffect, 524), CSPFT_FX },
  { "worldShellEjectEffect", WEAPON_FIELD(worldShellEjectEffect, 528), CSPFT_FX },
  { "viewLastShotEjectEffect", WEAPON_FIELD(viewLastShotEjectEffect, 532), CSPFT_FX },
  { "worldLastShotEjectEffect", WEAPON_FIELD(worldLastShotEjectEffect, 536), CSPFT_FX },
  { "reticleCenter", WEAPON_FIELD(reticleCenter, 540), CSPFT_MATERIAL },
  { "reticleSide", WEAPON_FIELD(reticleSide, 544), CSPFT_MATERIAL },
  { "reticleCenterSize", WEAPON_FIELD(iReticleCenterSize, 548), CSPFT_INT },
  { "reticleSideSize", WEAPON_FIELD(iReticleSideSize, 552), CSPFT_INT },
  { "reticleMinOfs", WEAPON_FIELD(iReticleMinOfs, 556), CSPFT_INT },
  { "activeReticleType", WEAPON_FIELD(activeReticleType, 560), WFT_ACTIVE_RETICLE_TYPE },
  { "standMoveF", WEAPON_FIELD(vStandMove[0], 564), CSPFT_FLOAT },
  { "standMoveR", WEAPON_FIELD(vStandMove[1], 568), CSPFT_FLOAT },
  { "standMoveU", WEAPON_FIELD(vStandMove[2], 572), CSPFT_FLOAT },
  { "standRotP", WEAPON_FIELD(vStandRot[0], 576), CSPFT_FLOAT },
  { "standRotY", WEAPON_FIELD(vStandRot[1], 580), CSPFT_FLOAT },
  { "standRotR", WEAPON_FIELD(vStandRot[2], 584), CSPFT_FLOAT },
  { "duckedOfsF", WEAPON_FIELD(vDuckedOfs[0], 588), CSPFT_FLOAT },
  { "duckedOfsR", WEAPON_FIELD(vDuckedOfs[1], 592), CSPFT_FLOAT },
  { "duckedOfsU", WEAPON_FIELD(vDuckedOfs[2], 596), CSPFT_FLOAT },
  { "duckedMoveF", WEAPON_FIELD(vDuckedMove[0], 600), CSPFT_FLOAT },
  { "duckedMoveR", WEAPON_FIELD(vDuckedMove[1], 604), CSPFT_FLOAT },
  { "duckedMoveU", WEAPON_FIELD(vDuckedMove[2], 608), CSPFT_FLOAT },
  { "duckedRotP", WEAPON_FIELD(vDuckedRot[0], 612), CSPFT_FLOAT },
  { "duckedRotY", WEAPON_FIELD(vDuckedRot[1], 616), CSPFT_FLOAT },
  { "duckedRotR", WEAPON_FIELD(vDuckedRot[2], 620), CSPFT_FLOAT },
  { "proneOfsF", WEAPON_FIELD(vProneOfs[0], 624), CSPFT_FLOAT },
  { "proneOfsR", WEAPON_FIELD(vProneOfs[1], 628), CSPFT_FLOAT },
  { "proneOfsU", WEAPON_FIELD(vProneOfs[2], 632), CSPFT_FLOAT },
  { "proneMoveF", WEAPON_FIELD(vProneMove[0], 636), CSPFT_FLOAT },
  { "proneMoveR", WEAPON_FIELD(vProneMove[1], 640), CSPFT_FLOAT },
  { "proneMoveU", WEAPON_FIELD(vProneMove[2], 644), CSPFT_FLOAT },
  { "proneRotP", WEAPON_FIELD(vProneRot[0], 648), CSPFT_FLOAT },
  { "proneRotY", WEAPON_FIELD(vProneRot[1], 652), CSPFT_FLOAT },
  { "proneRotR", WEAPON_FIELD(vProneRot[2], 656), CSPFT_FLOAT },
  { "posMoveRate", WEAPON_FIELD(fPosMoveRate, 660), CSPFT_FLOAT },
  { "posProneMoveRate", WEAPON_FIELD(fPosProneMoveRate, 664), CSPFT_FLOAT },
  { "standMoveMinSpeed", WEAPON_FIELD(fStandMoveMinSpeed, 668), CSPFT_FLOAT },
  { "duckedMoveMinSpeed", WEAPON_FIELD(fDuckedMoveMinSpeed, 672), CSPFT_FLOAT },
  { "proneMoveMinSpeed", WEAPON_FIELD(fProneMoveMinSpeed, 676), CSPFT_FLOAT },
  { "posRotRate", WEAPON_FIELD(fPosRotRate, 680), CSPFT_FLOAT },
  { "posProneRotRate", WEAPON_FIELD(fPosProneRotRate, 684), CSPFT_FLOAT },
  { "standRotMinSpeed", WEAPON_FIELD(fStandRotMinSpeed, 688), CSPFT_FLOAT },
  { "duckedRotMinSpeed", WEAPON_FIELD(fDuckedRotMinSpeed, 692), CSPFT_FLOAT },
  { "proneRotMinSpeed", WEAPON_FIELD(fProneRotMinSpeed, 696), CSPFT_FLOAT },
  { "worldModel", WEAPON_FIELD(worldModel[0], 700), CSPFT_XMODEL },
  { "worldModel2", WEAPON_FIELD(worldModel[1], 704), CSPFT_XMODEL },
  { "worldModel3", WEAPON_FIELD(worldModel[2], 708), CSPFT_XMODEL },
  { "worldModel4", WEAPON_FIELD(worldModel[3], 712), CSPFT_XMODEL },
  { "worldModel5", WEAPON_FIELD(worldModel[4], 716), CSPFT_XMODEL },
  { "worldModel6", WEAPON_FIELD(worldModel[5], 720), CSPFT_XMODEL },
  { "worldModel7", WEAPON_FIELD(worldModel[6], 724), CSPFT_XMODEL },
  { "worldModel8", WEAPON_FIELD(worldModel[7], 728), CSPFT_XMODEL },
  { "worldModel9", WEAPON_FIELD(worldModel[8], 732), CSPFT_XMODEL },
  { "worldModel10", WEAPON_FIELD(worldModel[9], 736), CSPFT_XMODEL },
  { "worldModel11", WEAPON_FIELD(worldModel[10], 740), CSPFT_XMODEL },
  { "worldModel12", WEAPON_FIELD(worldModel[11], 744), CSPFT_XMODEL },
  { "worldModel13", WEAPON_FIELD(worldModel[12], 748), CSPFT_XMODEL },
  { "worldModel14", WEAPON_FIELD(worldModel[13], 752), CSPFT_XMODEL },
  { "worldModel15", WEAPON_FIELD(worldModel[14], 756), CSPFT_XMODEL },
  { "worldModel16", WEAPON_FIELD(worldModel[15], 760), CSPFT_XMODEL },
  { "worldClipModel", WEAPON_FIELD(worldClipModel, 764), CSPFT_XMODEL },
  { "rocketModel", WEAPON_FIELD(rocketModel, 768), CSPFT_XMODEL },
  { "knifeModel", WEAPON_FIELD(knifeModel, 772), CSPFT_XMODEL },
  { "worldKnifeModel", WEAPON_FIELD(worldKnifeModel, 776), CSPFT_XMODEL },
  { "hudIcon", WEAPON_FIELD(hudIcon, 780), CSPFT_MATERIAL },
  { "hudIconRatio", WEAPON_FIELD(hudIconRatio, 784), WFT_ICONRATIO_HUD },
  { "ammoCounterIcon", WEAPON_FIELD(ammoCounterIcon, 788), CSPFT_MATERIAL },
  { "ammoCounterIconRatio", WEAPON_FIELD(ammoCounterIconRatio, 792), WFT_ICONRATIO_AMMOCOUNTER },
  { "ammoCounterClip", WEAPON_FIELD(ammoCounterClip, 796), WFT_AMMOCOUNTER_CLIPTYPE },
  { "startAmmo", WEAPON_FIELD(iStartAmmo, 800), CSPFT_INT },
  { "ammoName", WEAPON_FIELD(szAmmoName, 804), CSPFT_STRING },
  { "clipName", WEAPON_FIELD(szClipName, 812), CSPFT_STRING },
  { "maxAmmo", WEAPON_FIELD(iMaxAmmo, 820), CSPFT_INT },
  { "clipSize", WEAPON_FIELD(iClipSize, 824), CSPFT_INT },
  { "shotCount", WEAPON_FIELD(shotCount, 828), CSPFT_INT },
  { "sharedAmmoCapName", WEAPON_FIELD(szSharedAmmoCapName, 832), CSPFT_STRING },
  { "sharedAmmoCap", WEAPON_FIELD(iSharedAmmoCap, 840), CSPFT_INT },
  { "damage", WEAPON_FIELD(damage, 844), CSPFT_INT },
  { "playerDamage", WEAPON_FIELD(playerDamage, 848), CSPFT_INT },
  { "meleeDamage", WEAPON_FIELD(iMeleeDamage, 852), CSPFT_INT },
  { "minDamage", WEAPON_FIELD(minDamage, 2048), CSPFT_INT },
  { "minPlayerDamage", WEAPON_FIELD(minPlayerDamage, 2052), CSPFT_INT },
  { "maxDamageRange", WEAPON_FIELD(fMaxDamageRange, 2056), CSPFT_FLOAT },
  { "minDamageRange", WEAPON_FIELD(fMinDamageRange, 2060), CSPFT_FLOAT },
  { "destabilizationRateTime", WEAPON_FIELD(destabilizationRateTime, 2064), CSPFT_FLOAT },
  { "destabilizationCurvatureMax", WEAPON_FIELD(destabilizationCurvatureMax, 2068), CSPFT_FLOAT },
  { "destabilizeDistance", WEAPON_FIELD(destabilizeDistance, 2072), CSPFT_INT },
  { "fireDelay", WEAPON_FIELD(iFireDelay, 860), CSPFT_MILLISECONDS },
  { "meleeDelay", WEAPON_FIELD(iMeleeDelay, 864), CSPFT_MILLISECONDS },
  { "meleeChargeDelay", WEAPON_FIELD(meleeChargeDelay, 868), CSPFT_MILLISECONDS },
  { "fireTime", WEAPON_FIELD(iFireTime, 876), CSPFT_MILLISECONDS },
  { "rechamberTime", WEAPON_FIELD(iRechamberTime, 880), CSPFT_MILLISECONDS },
  { "rechamberBoltTime", WEAPON_FIELD(iRechamberBoltTime, 884), CSPFT_MILLISECONDS },
  { "holdFireTime", WEAPON_FIELD(iHoldFireTime, 888), CSPFT_MILLISECONDS },
  { "detonateTime", WEAPON_FIELD(iDetonateTime, 892), CSPFT_MILLISECONDS },
  { "detonateDelay", WEAPON_FIELD(iDetonateDelay, 872), CSPFT_MILLISECONDS },
  { "meleeTime", WEAPON_FIELD(iMeleeTime, 896), CSPFT_MILLISECONDS },
  { "meleeChargeTime", WEAPON_FIELD(meleeChargeTime, 900), CSPFT_MILLISECONDS },
  { "reloadTime", WEAPON_FIELD(iReloadTime, 904), CSPFT_MILLISECONDS },
  { "reloadShowRocketTime", WEAPON_FIELD(reloadShowRocketTime, 908), CSPFT_MILLISECONDS },
  { "reloadEmptyTime", WEAPON_FIELD(iReloadEmptyTime, 912), CSPFT_MILLISECONDS },
  { "reloadAddTime", WEAPON_FIELD(iReloadAddTime, 916), CSPFT_MILLISECONDS },
  { "reloadStartTime", WEAPON_FIELD(iReloadStartTime, 920), CSPFT_MILLISECONDS },
  { "reloadStartAddTime", WEAPON_FIELD(iReloadStartAddTime, 924), CSPFT_MILLISECONDS },
  { "reloadEndTime", WEAPON_FIELD(iReloadEndTime, 928), CSPFT_MILLISECONDS },
  { "dropTime", WEAPON_FIELD(iDropTime, 932), CSPFT_MILLISECONDS },
  { "raiseTime", WEAPON_FIELD(iRaiseTime, 936), CSPFT_MILLISECONDS },
  { "altDropTime", WEAPON_FIELD(iAltDropTime, 940), CSPFT_MILLISECONDS },
  { "altRaiseTime", WEAPON_FIELD(iAltRaiseTime, 944), CSPFT_MILLISECONDS },
  { "quickDropTime", WEAPON_FIELD(quickDropTime, 948), CSPFT_MILLISECONDS },
  { "quickRaiseTime", WEAPON_FIELD(quickRaiseTime, 952), CSPFT_MILLISECONDS },
  { "firstRaiseTime", WEAPON_FIELD(iFirstRaiseTime, 956), CSPFT_MILLISECONDS },
  { "emptyRaiseTime", WEAPON_FIELD(iEmptyRaiseTime, 960), CSPFT_MILLISECONDS },
  { "emptyDropTime", WEAPON_FIELD(iEmptyDropTime, 964), CSPFT_MILLISECONDS },
  { "sprintInTime", WEAPON_FIELD(sprintInTime, 968), CSPFT_MILLISECONDS },
  { "sprintLoopTime", WEAPON_FIELD(sprintLoopTime, 972), CSPFT_MILLISECONDS },
  { "sprintOutTime", WEAPON_FIELD(sprintOutTime, 976), CSPFT_MILLISECONDS },
  { "nightVisionWearTime", WEAPON_FIELD(nightVisionWearTime, 980), CSPFT_MILLISECONDS },
  { "nightVisionWearTimeFadeOutEnd", WEAPON_FIELD(nightVisionWearTimeFadeOutEnd, 984), CSPFT_MILLISECONDS },
  { "nightVisionWearTimePowerUp", WEAPON_FIELD(nightVisionWearTimePowerUp, 988), CSPFT_MILLISECONDS },
  { "nightVisionRemoveTime", WEAPON_FIELD(nightVisionRemoveTime, 992), CSPFT_MILLISECONDS },
  { "nightVisionRemoveTimePowerDown", WEAPON_FIELD(nightVisionRemoveTimePowerDown, 996), CSPFT_MILLISECONDS },
  { "nightVisionRemoveTimeFadeInStart", WEAPON_FIELD(nightVisionRemoveTimeFadeInStart, 1000), CSPFT_MILLISECONDS },
  { "fuseTime", WEAPON_FIELD(fuseTime, 1004), CSPFT_MILLISECONDS },
  { "aifuseTime", WEAPON_FIELD(aiFuseTime, 1008), CSPFT_MILLISECONDS },
  { "requireLockonToFire", WEAPON_FIELD(requireLockonToFire, 1012), CSPFT_QBOOLEAN },
  { "noAdsWhenMagEmpty", WEAPON_FIELD(noAdsWhenMagEmpty, 1016), CSPFT_QBOOLEAN },
  { "avoidDropCleanup", WEAPON_FIELD(avoidDropCleanup, 1020), CSPFT_QBOOLEAN },
  { "autoAimRange", WEAPON_FIELD(autoAimRange, 1024), CSPFT_FLOAT },
  { "aimAssistRange", WEAPON_FIELD(aimAssistRange, 1028), CSPFT_FLOAT },
  { "aimAssistRangeAds", WEAPON_FIELD(aimAssistRangeAds, 1032), CSPFT_FLOAT },
  { "aimPadding", WEAPON_FIELD(aimPadding, 1036), CSPFT_FLOAT },
  { "enemyCrosshairRange", WEAPON_FIELD(enemyCrosshairRange, 1040), CSPFT_FLOAT },
  { "crosshairColorChange", WEAPON_FIELD(crosshairColorChange, 1044), CSPFT_QBOOLEAN },
  { "moveSpeedScale", WEAPON_FIELD(moveSpeedScale, 1048), CSPFT_FLOAT },
  { "adsMoveSpeedScale", WEAPON_FIELD(adsMoveSpeedScale, 1052), CSPFT_FLOAT },
  { "sprintDurationScale", WEAPON_FIELD(sprintDurationScale, 1056), CSPFT_FLOAT },
  { "idleCrouchFactor", WEAPON_FIELD(fIdleCrouchFactor, 1180), CSPFT_FLOAT },
  { "idleProneFactor", WEAPON_FIELD(fIdleProneFactor, 1184), CSPFT_FLOAT },
  { "gunMaxPitch", WEAPON_FIELD(fGunMaxPitch, 1188), CSPFT_FLOAT },
  { "gunMaxYaw", WEAPON_FIELD(fGunMaxYaw, 1192), CSPFT_FLOAT },
  { "swayMaxAngle", WEAPON_FIELD(swayMaxAngle, 1196), CSPFT_FLOAT },
  { "swayLerpSpeed", WEAPON_FIELD(swayLerpSpeed, 1200), CSPFT_FLOAT },
  { "swayPitchScale", WEAPON_FIELD(swayPitchScale, 1204), CSPFT_FLOAT },
  { "swayYawScale", WEAPON_FIELD(swayYawScale, 1208), CSPFT_FLOAT },
  { "swayHorizScale", WEAPON_FIELD(swayHorizScale, 1212), CSPFT_FLOAT },
  { "swayVertScale", WEAPON_FIELD(swayVertScale, 1216), CSPFT_FLOAT },
  { "swayShellShockScale", WEAPON_FIELD(swayShellShockScale, 1220), CSPFT_FLOAT },
  { "adsSwayMaxAngle", WEAPON_FIELD(adsSwayMaxAngle, 1224), CSPFT_FLOAT },
  { "adsSwayLerpSpeed", WEAPON_FIELD(adsSwayLerpSpeed, 1228), CSPFT_FLOAT },
  { "adsSwayPitchScale", WEAPON_FIELD(adsSwayPitchScale, 1232), CSPFT_FLOAT },
  { "adsSwayYawScale", WEAPON_FIELD(adsSwayYawScale, 1236), CSPFT_FLOAT },
  { "adsSwayHorizScale", WEAPON_FIELD(adsSwayHorizScale, 1240), CSPFT_FLOAT },
  { "adsSwayVertScale", WEAPON_FIELD(adsSwayVertScale, 1244), CSPFT_FLOAT },
  { "rifleBullet", WEAPON_FIELD(bRifleBullet, 1248), CSPFT_QBOOLEAN },
  { "armorPiercing", WEAPON_FIELD(armorPiercing, 1252), CSPFT_QBOOLEAN },
  { "boltAction", WEAPON_FIELD(bBoltAction, 1256), CSPFT_QBOOLEAN },
  { "aimDownSight", WEAPON_FIELD(aimDownSight, 1260), CSPFT_QBOOLEAN },
  { "rechamberWhileAds", WEAPON_FIELD(bRechamberWhileAds, 1264), CSPFT_QBOOLEAN },
  { "adsViewErrorMin", WEAPON_FIELD(adsViewErrorMin, 1268), CSPFT_FLOAT },
  { "adsViewErrorMax", WEAPON_FIELD(adsViewErrorMax, 1272), CSPFT_FLOAT },
  { "clipOnly", WEAPON_FIELD(bClipOnly, 1280), CSPFT_QBOOLEAN },
  { "cookOffHold", WEAPON_FIELD(bCookOffHold, 1276), CSPFT_QBOOLEAN },
  { "adsFire", WEAPON_FIELD(adsFireOnly, 1284), CSPFT_QBOOLEAN },
  { "cancelAutoHolsterWhenEmpty", WEAPON_FIELD(cancelAutoHolsterWhenEmpty, 1288), CSPFT_QBOOLEAN },
  { "suppressAmmoReserveDisplay", WEAPON_FIELD(suppressAmmoReserveDisplay, 1292), CSPFT_QBOOLEAN },
  { "enhanced", WEAPON_FIELD(enhanced, 1296), CSPFT_QBOOLEAN },
  { "laserSightDuringNightvision", WEAPON_FIELD(laserSightDuringNightvision, 1300), CSPFT_QBOOLEAN },
  { "killIcon", WEAPON_FIELD(killIcon, 1304), CSPFT_MATERIAL },
  { "killIconRatio", WEAPON_FIELD(killIconRatio, 1308), WFT_ICONRATIO_KILL },
  { "flipKillIcon", WEAPON_FIELD(flipKillIcon, 1312), CSPFT_QBOOLEAN },
  { "dpadIcon", WEAPON_FIELD(dpadIcon, 1316), CSPFT_MATERIAL },
  { "dpadIconRatio", WEAPON_FIELD(dpadIconRatio, 1320), WFT_ICONRATIO_DPAD },
  { "noPartialReload", WEAPON_FIELD(bNoPartialReload, 1324), CSPFT_QBOOLEAN },
  { "segmentedReload", WEAPON_FIELD(bSegmentedReload, 1328), CSPFT_QBOOLEAN },
  { "reloadAmmoAdd", WEAPON_FIELD(iReloadAmmoAdd, 1332), CSPFT_INT },
  { "reloadStartAdd", WEAPON_FIELD(iReloadStartAdd, 1336), CSPFT_INT },
  { "altWeapon", WEAPON_FIELD(szAltWeaponName, 1340), CSPFT_STRING },
  { "dropAmmoMin", WEAPON_FIELD(iDropAmmoMin, 1348), CSPFT_INT },
  { "dropAmmoMax", WEAPON_FIELD(iDropAmmoMax, 1352), CSPFT_INT },
  { "blocksProne", WEAPON_FIELD(blocksProne, 1356), CSPFT_QBOOLEAN },
  { "silenced", WEAPON_FIELD(silenced, 1360), CSPFT_QBOOLEAN },
  { "explosionRadius", WEAPON_FIELD(iExplosionRadius, 1364), CSPFT_INT },
  { "explosionRadiusMin", WEAPON_FIELD(iExplosionRadiusMin, 1368), CSPFT_INT },
  { "explosionInnerDamage", WEAPON_FIELD(iExplosionInnerDamage, 1372), CSPFT_INT },
  { "explosionOuterDamage", WEAPON_FIELD(iExplosionOuterDamage, 1376), CSPFT_INT },
  { "damageConeAngle", WEAPON_FIELD(damageConeAngle, 1380), CSPFT_FLOAT },
  { "projectileSpeed", WEAPON_FIELD(iProjectileSpeed, 1384), CSPFT_INT },
  { "projectileSpeedUp", WEAPON_FIELD(iProjectileSpeedUp, 1388), CSPFT_INT },
  { "projectileSpeedForward", WEAPON_FIELD(iProjectileSpeedForward, 1392), CSPFT_INT },
  { "projectileActivateDist", WEAPON_FIELD(iProjectileActivateDist, 1396), CSPFT_INT },
  { "projectileLifetime", WEAPON_FIELD(projLifetime, 1400), CSPFT_FLOAT },
  { "timeToAccelerate", WEAPON_FIELD(timeToAccelerate, 1404), CSPFT_FLOAT },
  { "projectileCurvature", WEAPON_FIELD(projectileCurvature, 1408), CSPFT_FLOAT },
  { "projectileModel", WEAPON_FIELD(projectileModel, 1412), CSPFT_XMODEL },
  { "projExplosionType", WEAPON_FIELD(projExplosion, 1416), WFT_PROJ_EXPLOSION },
  { "projExplosionEffect", WEAPON_FIELD(projExplosionEffect, 1420), CSPFT_FX },
  { "projExplosionEffectForceNormalUp", WEAPON_FIELD(projExplosionEffectForceNormalUp, 1424), CSPFT_QBOOLEAN },
  { "projExplosionSound", WEAPON_FIELD(projExplosionSound, 1432), CSPFT_SOUND },
  { "projDudEffect", WEAPON_FIELD(projDudEffect, 1428), CSPFT_FX },
  { "projDudSound", WEAPON_FIELD(projDudSound, 1436), CSPFT_SOUND },
  { "projImpactExplode", WEAPON_FIELD(bProjImpactExplode, 1440), CSPFT_QBOOLEAN },
  { "stickiness", WEAPON_FIELD(stickiness, 1444), WFT_STICKINESS },
  { "hasDetonator", WEAPON_FIELD(hasDetonator, 1448), CSPFT_QBOOLEAN },
  { "timedDetonation", WEAPON_FIELD(timedDetonation, 1452), CSPFT_QBOOLEAN },
  { "rotate", WEAPON_FIELD(rotate, 1456), CSPFT_QBOOLEAN },
  { "holdButtonToThrow", WEAPON_FIELD(holdButtonToThrow, 1460), CSPFT_QBOOLEAN },
  { "freezeMovementWhenFiring", WEAPON_FIELD(freezeMovementWhenFiring, 1464), CSPFT_QBOOLEAN },
  { "lowAmmoWarningThreshold", WEAPON_FIELD(lowAmmoWarningThreshold, 1468), CSPFT_FLOAT },
  { "parallelDefaultBounce", WEAPON_FIELD(parallelBounce[0], 1472), CSPFT_FLOAT },
  { "parallelBarkBounce", WEAPON_FIELD(parallelBounce[1], 1476), CSPFT_FLOAT },
  { "parallelBrickBounce", WEAPON_FIELD(parallelBounce[2], 1480), CSPFT_FLOAT },
  { "parallelCarpetBounce", WEAPON_FIELD(parallelBounce[3], 1484), CSPFT_FLOAT },
  { "parallelClothBounce", WEAPON_FIELD(parallelBounce[4], 1488), CSPFT_FLOAT },
  { "parallelConcreteBounce", WEAPON_FIELD(parallelBounce[5], 1492), CSPFT_FLOAT },
  { "parallelDirtBounce", WEAPON_FIELD(parallelBounce[6], 1496), CSPFT_FLOAT },
  { "parallelFleshBounce", WEAPON_FIELD(parallelBounce[7], 1500), CSPFT_FLOAT },
  { "parallelFoliageBounce", WEAPON_FIELD(parallelBounce[8], 1504), CSPFT_FLOAT },
  { "parallelGlassBounce", WEAPON_FIELD(parallelBounce[9], 1508), CSPFT_FLOAT },
  { "parallelGrassBounce", WEAPON_FIELD(parallelBounce[10], 1512), CSPFT_FLOAT },
  { "parallelGravelBounce", WEAPON_FIELD(parallelBounce[11], 1516), CSPFT_FLOAT },
  { "parallelIceBounce", WEAPON_FIELD(parallelBounce[12], 1520), CSPFT_FLOAT },
  { "parallelMetalBounce", WEAPON_FIELD(parallelBounce[13], 1524), CSPFT_FLOAT },
  { "parallelMudBounce", WEAPON_FIELD(parallelBounce[14], 1528), CSPFT_FLOAT },
  { "parallelPaperBounce", WEAPON_FIELD(parallelBounce[15], 1532), CSPFT_FLOAT },
  { "parallelPlasterBounce", WEAPON_FIELD(parallelBounce[16], 1536), CSPFT_FLOAT },
  { "parallelRockBounce", WEAPON_FIELD(parallelBounce[17], 1540), CSPFT_FLOAT },
  { "parallelSandBounce", WEAPON_FIELD(parallelBounce[18], 1544), CSPFT_FLOAT },
  { "parallelSnowBounce", WEAPON_FIELD(parallelBounce[19], 1548), CSPFT_FLOAT },
  { "parallelWaterBounce", WEAPON_FIELD(parallelBounce[20], 1552), CSPFT_FLOAT },
  { "parallelWoodBounce", WEAPON_FIELD(parallelBounce[21], 1556), CSPFT_FLOAT },
  { "parallelAsphaltBounce", WEAPON_FIELD(parallelBounce[22], 1560), CSPFT_FLOAT },
  { "parallelCeramicBounce", WEAPON_FIELD(parallelBounce[23], 1564), CSPFT_FLOAT },
  { "parallelPlasticBounce", WEAPON_FIELD(parallelBounce[24], 1568), CSPFT_FLOAT },
  { "parallelRubberBounce", WEAPON_FIELD(parallelBounce[25], 1572), CSPFT_FLOAT },
  { "parallelCushionBounce", WEAPON_FIELD(parallelBounce[26], 1576), CSPFT_FLOAT },
  { "parallelFruitBounce", WEAPON_FIELD(parallelBounce[27], 1580), CSPFT_FLOAT },
  { "parallelPaintedMetalBounce", WEAPON_FIELD(parallelBounce[28], 1584), CSPFT_FLOAT },
  { "perpendicularDefaultBounce", WEAPON_FIELD(perpendicularBounce[0], 1588), CSPFT_FLOAT },
  { "perpendicularBarkBounce", WEAPON_FIELD(perpendicularBounce[1], 1592), CSPFT_FLOAT },
  { "perpendicularBrickBounce", WEAPON_FIELD(perpendicularBounce[2], 1596), CSPFT_FLOAT },
  { "perpendicularCarpetBounce", WEAPON_FIELD(perpendicularBounce[3], 1600), CSPFT_FLOAT },
  { "perpendicularClothBounce", WEAPON_FIELD(perpendicularBounce[4], 1604), CSPFT_FLOAT },
  { "perpendicularConcreteBounce", WEAPON_FIELD(perpendicularBounce[5], 1608), CSPFT_FLOAT },
  { "perpendicularDirtBounce", WEAPON_FIELD(perpendicularBounce[6], 1612), CSPFT_FLOAT },
  { "perpendicularFleshBounce", WEAPON_FIELD(perpendicularBounce[7], 1616), CSPFT_FLOAT },
  { "perpendicularFoliageBounce", WEAPON_FIELD(perpendicularBounce[8], 1620), CSPFT_FLOAT },
  { "perpendicularGlassBounce", WEAPON_FIELD(perpendicularBounce[9], 1624), CSPFT_FLOAT },
  { "perpendicularGrassBounce", WEAPON_FIELD(perpendicularBounce[10], 1628), CSPFT_FLOAT },
  { "perpendicularGravelBounce", WEAPON_FIELD(perpendicularBounce[11], 1632), CSPFT_FLOAT },
  { "perpendicularIceBounce", WEAPON_FIELD(perpendicularBounce[12], 1636), CSPFT_FLOAT },
  { "perpendicularMetalBounce", WEAPON_FIELD(perpendicularBounce[13], 1640), CSPFT_FLOAT },
  { "perpendicularMudBounce", WEAPON_FIELD(perpendicularBounce[14], 1644), CSPFT_FLOAT },
  { "perpendicularPaperBounce", WEAPON_FIELD(perpendicularBounce[15], 1648), CSPFT_FLOAT },
  { "perpendicularPlasterBounce", WEAPON_FIELD(perpendicularBounce[16], 1652), CSPFT_FLOAT },
  { "perpendicularRockBounce", WEAPON_FIELD(perpendicularBounce[17], 1656), CSPFT_FLOAT },
  { "perpendicularSandBounce", WEAPON_FIELD(perpendicularBounce[18], 1660), CSPFT_FLOAT },
  { "perpendicularSnowBounce", WEAPON_FIELD(perpendicularBounce[19], 1664), CSPFT_FLOAT },
  { "perpendicularWaterBounce", WEAPON_FIELD(perpendicularBounce[20], 1668), CSPFT_FLOAT },
  { "perpendicularWoodBounce", WEAPON_FIELD(perpendicularBounce[21], 1672), CSPFT_FLOAT },
  { "perpendicularAsphaltBounce", WEAPON_FIELD(perpendicularBounce[22], 1676), CSPFT_FLOAT },
  { "perpendicularCeramicBounce", WEAPON_FIELD(parallelBounce[23], 1564), CSPFT_FLOAT },
  { "perpendicularPlasticBounce", WEAPON_FIELD(parallelBounce[24], 1568), CSPFT_FLOAT },
  { "perpendicularRubberBounce", WEAPON_FIELD(parallelBounce[25], 1572), CSPFT_FLOAT },
  { "perpendicularCushionBounce", WEAPON_FIELD(perpendicularBounce[26], 1692), CSPFT_FLOAT },
  { "perpendicularFruitBounce", WEAPON_FIELD(perpendicularBounce[27], 1696), CSPFT_FLOAT },
  { "perpendicularPaintedMetalBounce", WEAPON_FIELD(perpendicularBounce[28], 1700), CSPFT_FLOAT },
  { "projTrailEffect", WEAPON_FIELD(projTrailEffect, 1704), CSPFT_FX },
  { "projectileRed", WEAPON_FIELD(vProjectileColor[0], 1708), CSPFT_FLOAT },
  { "projectileGreen", WEAPON_FIELD(vProjectileColor[1], 1712), CSPFT_FLOAT },
  { "projectileBlue", WEAPON_FIELD(vProjectileColor[2], 1716), CSPFT_FLOAT },
  { "guidedMissileType", WEAPON_FIELD(guidedMissileType, 1720), WFT_GUIDED_MISSILE_TYPE },
  { "maxSteeringAccel", WEAPON_FIELD(maxSteeringAccel, 1724), CSPFT_FLOAT },
  { "projIgnitionDelay", WEAPON_FIELD(projIgnitionDelay, 1728), CSPFT_INT },
  { "projIgnitionEffect", WEAPON_FIELD(projIgnitionEffect, 1732), CSPFT_FX },
  { "projIgnitionSound", WEAPON_FIELD(projIgnitionSound, 1736), CSPFT_SOUND },
  { "adsTransInTime", WEAPON_FIELD(iAdsTransInTime, 1156), CSPFT_MILLISECONDS },
  { "adsTransOutTime", WEAPON_FIELD(iAdsTransOutTime, 1160), CSPFT_MILLISECONDS },
  { "adsIdleAmount", WEAPON_FIELD(fAdsIdleAmount, 1164), CSPFT_FLOAT },
  { "adsIdleSpeed", WEAPON_FIELD(adsIdleSpeed, 1172), CSPFT_FLOAT },
  { "adsZoomFov", WEAPON_FIELD(fAdsZoomFov, 1060), CSPFT_FLOAT },
  { "adsZoomInFrac", WEAPON_FIELD(fAdsZoomInFrac, 1064), CSPFT_FLOAT },
  { "adsZoomOutFrac", WEAPON_FIELD(fAdsZoomOutFrac, 1068), CSPFT_FLOAT },
  { "adsOverlayShader", WEAPON_FIELD(overlayMaterial, 1072), CSPFT_MATERIAL },
  { "adsOverlayShaderLowRes", WEAPON_FIELD(overlayMaterialLowRes, 1076), CSPFT_MATERIAL },
  { "adsOverlayReticle", WEAPON_FIELD(overlayReticle, 1080), WFT_OVERLAYRETICLE },
  { "adsOverlayInterface", WEAPON_FIELD(overlayInterface, 1084), WFT_OVERLAYINTERFACE },
  { "adsOverlayWidth", WEAPON_FIELD(overlayWidth, 1088), CSPFT_FLOAT },
  { "adsOverlayHeight", WEAPON_FIELD(overlayHeight, 1092), CSPFT_FLOAT },
  { "adsBobFactor", WEAPON_FIELD(fAdsBobFactor, 1096), CSPFT_FLOAT },
  { "adsViewBobMult", WEAPON_FIELD(fAdsViewBobMult, 1100), CSPFT_FLOAT },
  { "adsAimPitch", WEAPON_FIELD(fAdsAimPitch, 1740), CSPFT_FLOAT },
  { "adsCrosshairInFrac", WEAPON_FIELD(fAdsCrosshairInFrac, 1744), CSPFT_FLOAT },
  { "adsCrosshairOutFrac", WEAPON_FIELD(fAdsCrosshairOutFrac, 1748), CSPFT_FLOAT },
  { "adsReloadTransTime", WEAPON_FIELD(iPositionReloadTransTime, 1940), CSPFT_MILLISECONDS },
  { "adsGunKickReducedKickBullets", WEAPON_FIELD(adsGunKickReducedKickBullets, 1752), CSPFT_INT },
  { "adsGunKickReducedKickPercent", WEAPON_FIELD(adsGunKickReducedKickPercent, 1756), CSPFT_FLOAT },
  { "adsGunKickPitchMin", WEAPON_FIELD(fAdsGunKickPitchMin, 1760), CSPFT_FLOAT },
  { "adsGunKickPitchMax", WEAPON_FIELD(fAdsGunKickPitchMax, 1764), CSPFT_FLOAT },
  { "adsGunKickYawMin", WEAPON_FIELD(fAdsGunKickYawMin, 1768), CSPFT_FLOAT },
  { "adsGunKickYawMax", WEAPON_FIELD(fAdsGunKickYawMax, 1772), CSPFT_FLOAT },
  { "adsGunKickAccel", WEAPON_FIELD(fAdsGunKickAccel, 1776), CSPFT_FLOAT },
  { "adsGunKickSpeedMax", WEAPON_FIELD(fAdsGunKickSpeedMax, 1780), CSPFT_FLOAT },
  { "adsGunKickSpeedDecay", WEAPON_FIELD(fAdsGunKickSpeedDecay, 1784), CSPFT_FLOAT },
  { "adsGunKickStaticDecay", WEAPON_FIELD(fAdsGunKickStaticDecay, 1788), CSPFT_FLOAT },
  { "adsViewKickPitchMin", WEAPON_FIELD(fAdsViewKickPitchMin, 1792), CSPFT_FLOAT },
  { "adsViewKickPitchMax", WEAPON_FIELD(fAdsViewKickPitchMax, 1796), CSPFT_FLOAT },
  { "adsViewKickYawMin", WEAPON_FIELD(fAdsViewKickYawMin, 1800), CSPFT_FLOAT },
  { "adsViewKickYawMax", WEAPON_FIELD(fAdsViewKickYawMax, 1804), CSPFT_FLOAT },
  { "adsViewKickCenterSpeed", WEAPON_FIELD(fAdsViewKickCenterSpeed, 1808), CSPFT_FLOAT },
  { "adsSpread", WEAPON_FIELD(fAdsSpread, 1820), CSPFT_FLOAT },
  { "guidedMissileType", WEAPON_FIELD(guidedMissileType, 1720), WFT_GUIDED_MISSILE_TYPE },
  { "hipSpreadStandMin", WEAPON_FIELD(fHipSpreadStandMin, 1104), CSPFT_FLOAT },
  { "hipSpreadDuckedMin", WEAPON_FIELD(fHipSpreadDuckedMin, 1108), CSPFT_FLOAT },
  { "hipSpreadProneMin", WEAPON_FIELD(fHipSpreadProneMin, 1112), CSPFT_FLOAT },
  { "hipSpreadMax", WEAPON_FIELD(hipSpreadStandMax, 1116), CSPFT_FLOAT },
  { "hipSpreadDuckedMax", WEAPON_FIELD(hipSpreadDuckedMax, 1120), CSPFT_FLOAT },
  { "hipSpreadProneMax", WEAPON_FIELD(hipSpreadProneMax, 1124), CSPFT_FLOAT },
  { "hipSpreadDecayRate", WEAPON_FIELD(fHipSpreadDecayRate, 1128), CSPFT_FLOAT },
  { "hipSpreadFireAdd", WEAPON_FIELD(fHipSpreadFireAdd, 1132), CSPFT_FLOAT },
  { "hipSpreadTurnAdd", WEAPON_FIELD(fHipSpreadTurnAdd, 1136), CSPFT_FLOAT },
  { "hipSpreadMoveAdd", WEAPON_FIELD(fHipSpreadMoveAdd, 1140), CSPFT_FLOAT },
  { "hipSpreadDuckedDecay", WEAPON_FIELD(fHipSpreadDuckedDecay, 1144), CSPFT_FLOAT },
  { "hipSpreadProneDecay", WEAPON_FIELD(fHipSpreadProneDecay, 1148), CSPFT_FLOAT },
  { "hipReticleSidePos", WEAPON_FIELD(fHipReticleSidePos, 1152), CSPFT_FLOAT },
  { "hipIdleAmount", WEAPON_FIELD(fHipIdleAmount, 1168), CSPFT_FLOAT },
  { "hipIdleSpeed", WEAPON_FIELD(hipIdleSpeed, 1176), CSPFT_FLOAT },
  { "hipGunKickReducedKickBullets", WEAPON_FIELD(hipGunKickReducedKickBullets, 1824), CSPFT_INT },
  { "hipGunKickReducedKickPercent", WEAPON_FIELD(hipGunKickReducedKickPercent, 1828), CSPFT_FLOAT },
  { "hipGunKickPitchMin", WEAPON_FIELD(fHipGunKickPitchMin, 1832), CSPFT_FLOAT },
  { "hipGunKickPitchMax", WEAPON_FIELD(fHipGunKickPitchMax, 1836), CSPFT_FLOAT },
  { "hipGunKickYawMin", WEAPON_FIELD(fHipGunKickYawMin, 1840), CSPFT_FLOAT },
  { "hipGunKickYawMax", WEAPON_FIELD(fHipGunKickYawMax, 1844), CSPFT_FLOAT },
  { "hipGunKickAccel", WEAPON_FIELD(fHipGunKickAccel, 1848), CSPFT_FLOAT },
  { "hipGunKickSpeedMax", WEAPON_FIELD(fHipGunKickSpeedMax, 1852), CSPFT_FLOAT },
  { "hipGunKickSpeedDecay", WEAPON_FIELD(fHipGunKickSpeedDecay, 1856), CSPFT_FLOAT },
  { "hipGunKickStaticDecay", WEAPON_FIELD(fHipGunKickStaticDecay, 1860), CSPFT_FLOAT },
  { "hipViewKickPitchMin", WEAPON_FIELD(fHipViewKickPitchMin, 1864), CSPFT_FLOAT },
  { "hipViewKickPitchMax", WEAPON_FIELD(fHipViewKickPitchMax, 1868), CSPFT_FLOAT },
  { "hipViewKickYawMin", WEAPON_FIELD(fHipViewKickYawMin, 1872), CSPFT_FLOAT },
  { "hipViewKickYawMax", WEAPON_FIELD(fHipViewKickYawMax, 1876), CSPFT_FLOAT },
  { "hipViewKickCenterSpeed", WEAPON_FIELD(fHipViewKickCenterSpeed, 1880), CSPFT_FLOAT },
  { "leftArc", WEAPON_FIELD(leftArc, 1944), CSPFT_FLOAT },
  { "rightArc", WEAPON_FIELD(rightArc, 1948), CSPFT_FLOAT },
  { "topArc", WEAPON_FIELD(topArc, 1952), CSPFT_FLOAT },
  { "bottomArc", WEAPON_FIELD(bottomArc, 1956), CSPFT_FLOAT },
  { "accuracy", WEAPON_FIELD(accuracy, 1960), CSPFT_FLOAT },
  { "aiSpread", WEAPON_FIELD(aiSpread, 1964), CSPFT_FLOAT },
  { "playerSpread", WEAPON_FIELD(playerSpread, 1968), CSPFT_FLOAT },
  { "maxVertTurnSpeed", WEAPON_FIELD(maxTurnSpeed[0], 1980), CSPFT_FLOAT },
  { "maxHorTurnSpeed", WEAPON_FIELD(maxTurnSpeed[1], 1984), CSPFT_FLOAT },
  { "minVertTurnSpeed", WEAPON_FIELD(minTurnSpeed[0], 1972), CSPFT_FLOAT },
  { "minHorTurnSpeed", WEAPON_FIELD(minTurnSpeed[1], 1976), CSPFT_FLOAT },
  { "pitchConvergenceTime", WEAPON_FIELD(pitchConvergenceTime, 1988), CSPFT_FLOAT },
  { "yawConvergenceTime", WEAPON_FIELD(yawConvergenceTime, 1992), CSPFT_FLOAT },
  { "suppressionTime", WEAPON_FIELD(suppressTime, 1996), CSPFT_FLOAT },
  { "maxRange", WEAPON_FIELD(maxRange, 2000), CSPFT_FLOAT },
  { "animHorRotateInc", WEAPON_FIELD(fAnimHorRotateInc, 2004), CSPFT_FLOAT },
  { "playerPositionDist", WEAPON_FIELD(fPlayerPositionDist, 2008), CSPFT_FLOAT },
  { "stance", WEAPON_FIELD(stance, 328), WFT_STANCE },
  { "useHintString", WEAPON_FIELD(szUseHintString, 2012), CSPFT_STRING },
  { "dropHintString", WEAPON_FIELD(dropHintString, 2016), CSPFT_STRING },
  { "horizViewJitter", WEAPON_FIELD(horizViewJitter, 2028), CSPFT_FLOAT },
  { "vertViewJitter", WEAPON_FIELD(vertViewJitter, 2032), CSPFT_FLOAT },
  { "fightDist", WEAPON_FIELD(fightDist, 1892), CSPFT_FLOAT },
  { "maxDist", WEAPON_FIELD(maxDist, 1896), CSPFT_FLOAT },
  { "aiVsAiAccuracyGraph", WEAPON_FIELD(accuracyGraphName[0], 1900), CSPFT_STRING },
  { "aiVsPlayerAccuracyGraph", WEAPON_FIELD(accuracyGraphName[1], 1904), CSPFT_STRING },
  { "locNone", WEAPON_FIELD(locationDamageMultipliers[0], 2076), CSPFT_FLOAT },
  { "locHelmet", WEAPON_FIELD(locationDamageMultipliers[1], 2080), CSPFT_FLOAT },
  { "locHead", WEAPON_FIELD(locationDamageMultipliers[2], 2084), CSPFT_FLOAT },
  { "locNeck", WEAPON_FIELD(locationDamageMultipliers[3], 2088), CSPFT_FLOAT },
  { "locTorsoUpper", WEAPON_FIELD(locationDamageMultipliers[4], 2092), CSPFT_FLOAT },
  { "locTorsoLower", WEAPON_FIELD(locationDamageMultipliers[5], 2096), CSPFT_FLOAT },
  { "locRightArmUpper", WEAPON_FIELD(locationDamageMultipliers[6], 2100), CSPFT_FLOAT },
  { "locRightArmLower", WEAPON_FIELD(locationDamageMultipliers[8], 2108), CSPFT_FLOAT },
  { "locRightHand", WEAPON_FIELD(locationDamageMultipliers[10], 2116), CSPFT_FLOAT },
  { "locLeftArmUpper", WEAPON_FIELD(locationDamageMultipliers[7], 2104), CSPFT_FLOAT },
  { "locLeftArmLower", WEAPON_FIELD(locationDamageMultipliers[9], 2112), CSPFT_FLOAT },
  { "locLeftHand", WEAPON_FIELD(locationDamageMultipliers[11], 2120), CSPFT_FLOAT },
  { "locRightLegUpper", WEAPON_FIELD(locationDamageMultipliers[12], 2124), CSPFT_FLOAT },
  { "locRightLegLower", WEAPON_FIELD(locationDamageMultipliers[14], 2132), CSPFT_FLOAT },
  { "locRightFoot", WEAPON_FIELD(locationDamageMultipliers[16], 2140), CSPFT_FLOAT },
  { "locLeftLegUpper", WEAPON_FIELD(locationDamageMultipliers[13], 2128), CSPFT_FLOAT },
  { "locLeftLegLower", WEAPON_FIELD(locationDamageMultipliers[15], 2136), CSPFT_FLOAT },
  { "locLeftFoot", WEAPON_FIELD(locationDamageMultipliers[17], 2144), CSPFT_FLOAT },
  { "locGun", WEAPON_FIELD(locationDamageMultipliers[18], 2148), CSPFT_FLOAT },
  { "fireRumble", WEAPON_FIELD(fireRumble, 2152), CSPFT_STRING },
  { "meleeImpactRumble", WEAPON_FIELD(meleeImpactRumble, 2156), CSPFT_STRING },
  { "adsDofStart", WEAPON_FIELD(adsDofStart, 2160), CSPFT_FLOAT },
  { "adsDofEnd", WEAPON_FIELD(adsDofEnd, 2164), CSPFT_FLOAT }
}; // idb

// const char *szWeapTypeNames[4] = { "bullet", "grenade", "projectile", "binoculars" }; // idb
const char *szWeapClassNames[10] =
{
  "rifle",
  "mg",
  "smg",
  "spread",
  "pistol",
  "grenade",
  "rocketlauncher",
  "turret",
  "non-player",
  "item"
}; // idb

char *g_playerAnimTypeNames[64];

WeaponDef bg_defaultWeaponDefs;

char *BG_GetPlayerAnimTypeName(int32_t index)
{
    return g_playerAnimTypeNames[index];
}

void TRACK_bg_weapons_load_obj()
{
    track_static_alloc_internal(szWeapOverlayReticleNames, 8, "szWeapOverlayReticleNames", 9);
    track_static_alloc_internal(szWeapStanceNames, 12, "szWeapStanceNames", 9);
    track_static_alloc_internal(weaponDefFields, 6024, "weaponDefFields", 9);
    track_static_alloc_internal(&bg_defaultWeaponDefs, 2168, "bg_defaultWeaponDefs", 9);
    track_static_alloc_internal(penetrateTypeNames, 16, "penetrateTypeNames", 9);
    track_static_alloc_internal(szWeapTypeNames, 16, "szWeapTypeNames", 9);
    track_static_alloc_internal(szWeapClassNames, 40, "szWeapClassNames", 9);
    track_static_alloc_internal(g_playerAnimTypeNames, 256, "g_playerAnimTypeNames", 9);
    track_static_alloc_internal(szWeapInventoryTypeNames, 16, "szWeapInventoryTypeNames", 9);
}

const char *BG_GetWeaponTypeName(weapType_t type)
{
    bcassert(type < WEAPTYPE_NUM, ARRAY_COUNT(szWeapTypeNames));

    return szWeapTypeNames[type];
}

const char *BG_GetWeaponClassName(weapClass_t type)
{
    bcassert(type < WEAPCLASS_NUM, ARRAY_COUNT(szWeapClassNames));

    return szWeapClassNames[type];
}

const char *BG_GetWeaponInventoryTypeName(weapInventoryType_t type)
{
    bcassert(type < WEAPINVENTORYCOUNT, ARRAY_COUNT(szWeapInventoryTypeNames));

    return szWeapInventoryTypeNames[type];
}

#ifdef KISAK_MP
void BG_LoadWeaponStrings()
{
    uint32_t i; // [esp+0h] [ebp-4h]

    for (i = 0; i < g_playerAnimTypeNamesCount; ++i)
        BG_InitWeaponString(i, g_playerAnimTypeNames[i]);
}
#endif

void BG_LoadPlayerAnimTypes()
{
#ifdef KISAK_MP
    char v0; // [esp+3h] [ebp-29h]
    char *v1; // [esp+8h] [ebp-24h]
    const char *v2; // [esp+Ch] [ebp-20h]
    char *buf; // [esp+20h] [ebp-Ch]
    const char *text_p; // [esp+24h] [ebp-8h] BYREF
    const char *token; // [esp+28h] [ebp-4h]

    g_playerAnimTypeNamesCount = 0;
    buf = Com_LoadRawTextFile("mp/playeranimtypes.txt");
    if (!buf)
        Com_Error(ERR_DROP, "Couldn',27h,'t load file %s", "mp/playeranimtypes.txt");
    text_p = buf;
    Com_BeginParseSession("BG_AnimParseAnimScript");
    while (1)
    {
        token = (const char *)Com_Parse(&text_p);
        if (!token || !*token)
            break;
        if (g_playerAnimTypeNamesCount >= 0x40)
            Com_Error(ERR_DROP, "Player anim type array size exceeded");
        g_playerAnimTypeNames[g_playerAnimTypeNamesCount] = (char *)Hunk_Alloc(
            strlen(token) + 1,
            "BG_LoadPlayerAnimTypes",
            9);
        v2 = token;
        v1 = g_playerAnimTypeNames[g_playerAnimTypeNamesCount];
        do
        {
            v0 = *v2;
            *v1++ = *v2++;
        } while (v0);
        ++g_playerAnimTypeNamesCount;
    }
    Com_EndParseSession();
    Com_UnloadRawTextFile(buf);
#elif KISAK_SP
    g_playerAnimTypeNamesCount = 1;
    g_playerAnimTypeNames[0] = (char*)"none";
#endif
}

void InitWeaponDef(WeaponDef *weapDef)
{
    const cspField_t *pField; // [esp+4h] [ebp-8h]
    int iField; // [esp+8h] [ebp-4h]

    weapDef->szInternalName = "";
    iField = 0;
    pField = weaponDefFields;
    while (iField < 502)
    {
        if (pField->iFieldType == CSPFT_STRING)
            *(const char **)((char *)&weapDef->szInternalName + pField->iOffset) = "";
        ++iField;
        ++pField;
    }
}

char G_ParseAIWeaponAccurayGraphFile(
    const char *buffer,
    const char *fileName,
    float (*knots)[2],
    int *knotCount)
{
    int v4; // eax
    double v5; // st7
    double v6; // st7
    int knotCountIndex; // [esp+0h] [ebp-8h]
    parseInfo_t *tokenb; // [esp+4h] [ebp-4h]
    parseInfo_t *token; // [esp+4h] [ebp-4h]
    parseInfo_t *tokena; // [esp+4h] [ebp-4h]

    iassert(buffer);
    iassert(fileName);
    iassert(knots);
    iassert(knotCount);

    Com_BeginParseSession(fileName);
    tokenb = Com_Parse(&buffer);
    v4 = atoi(tokenb->token);
    *knotCount = v4;
    knotCountIndex = 0;
    while (1)
    {
        token = Com_Parse(&buffer);
        if (!token->token[0])
            break;
        if (token->token[0] == 125)
            break;
        v5 = atof(token->token);
        (*knots)[2 * knotCountIndex] = v5;
        tokena = Com_Parse(&buffer);
        if (!tokena->token[0] || tokena->token[0] == 125)
            break;
        v6 = atof(tokena->token);
        (*knots)[2 * knotCountIndex++ + 1] = v6;
        if (knotCountIndex >= 16)
        {
            Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" has too many graph knots\n", fileName);
            Com_EndParseSession();
            return 0;
        }
    }
    Com_EndParseSession();
    if (knotCountIndex == *knotCount)
    {
        if ((*knots)[2 * knotCountIndex - 2] == 1.0)
        {
            return 1;
        }
        else
        {
            Com_PrintError(CON_CHANNEL_SERVER, "ERROR: \"%s\" Range must be 0.0 to 1.0\n", fileName);
            return 0;
        }
    }
    else
    {
        Com_PrintError(CON_CHANNEL_SERVER, "ERROR: \"%s\" Error in parsing an ai weapon accuracy file\n", fileName);
        return 0;
    }
}

char G_ParseWeaponAccurayGraphInternal(
    WeaponDef *weaponDef,
    const char *dirName,
    const char *graphName,
    float (*knots)[2],
    int *knotCount)
{
    signed int v6; // [esp+10h] [ebp-205Ch]
    char string[64]; // [esp+14h] [ebp-2058h] BYREF
    char buffer[8196]; // [esp+54h] [ebp-2018h] BYREF
    const char *last; // [esp+205Ch] [ebp-10h]
    int knotCounta; // [esp+2060h] [ebp-Ch] BYREF
    int f; // [esp+2064h] [ebp-8h] BYREF
    int len; // [esp+2068h] [ebp-4h]

    last = "WEAPONACCUFILE";
    len = strlen("WEAPONACCUFILE");
    iassert(weaponDef);
    iassert(graphName);
    iassert(knots);
    iassert(knotCount);
    iassert(dirName);

    if (weaponDef->weapType != WEAPTYPE_BULLET && weaponDef->weapType != WEAPTYPE_PROJECTILE)
        return 1;

    if (!*graphName)
        return 1;

    snprintf(string, ARRAYSIZE(string), "accuracy/%s/%s", dirName, graphName);
    v6 = FS_FOpenFileByMode(string, &f, FS_READ);
    if (v6 >= 0)
    {
        FS_Read((uint8_t *)buffer, len, f);
        buffer[len] = 0;
        if (!strncmp(buffer, last, len))
        {
            if (v6 - len < 0x2000)
            {
                memset((uint8_t *)buffer, 0, 0x2000u);
                FS_Read((uint8_t *)buffer, v6 - len, f);
                buffer[v6 - len] = 0;
                FS_FCloseFile(f);
                knotCounta = 0;
                if (G_ParseAIWeaponAccurayGraphFile(buffer, string, knots, &knotCounta))
                {
                    *knotCount = knotCounta;
                    return 1;
                }
                else
                {
                    return 0;
                }
            }
            else
            {
                Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" Is too long of an ai weapon accuracy file to parse\n", string);
                FS_FCloseFile(f);
                return 0;
            }
        }
        else
        {
            Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" does not appear to be an ai weapon accuracy file\n", string);
            FS_FCloseFile(f);
            return 0;
        }
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: Could not load ai weapon accuracy file '%s'\n", string);
        return 0;
    }
}

char G_ParseWeaponAccurayGraphs(WeaponDef *weaponDef)
{
    uint32_t size; // [esp+4h] [ebp-8Ch]
    int weaponType; // [esp+8h] [ebp-88h]
    int accuracyGraphKnotCount; // [esp+Ch] [ebp-84h] BYREF
    float accuracyGraphKnots[16][2]; // [esp+10h] [ebp-80h] BYREF

    for (weaponType = 0; weaponType < 2; ++weaponType)
    {
        memset((uint8_t *)accuracyGraphKnots, 0, sizeof(accuracyGraphKnots));
        accuracyGraphKnotCount = 0;
        if (!G_ParseWeaponAccurayGraphInternal(
            weaponDef,
            accuracyDirName[weaponType],
            weaponDef->accuracyGraphName[weaponType],
            accuracyGraphKnots,
            &accuracyGraphKnotCount))
            return 0;
        if (accuracyGraphKnotCount > 0)
        {
            size = 8 * accuracyGraphKnotCount;
            weaponDef->accuracyGraphKnots[weaponType] = (float (*)[2])Hunk_AllocLowAlign(
                8 * accuracyGraphKnotCount,
                4,
                "G_ParseWeaponAccurayGraphs",
                9);
            weaponDef->originalAccuracyGraphKnots[weaponType] = weaponDef->accuracyGraphKnots[weaponType];
            memcpy((uint8_t *)weaponDef->accuracyGraphKnots[weaponType], (uint8_t *)accuracyGraphKnots, size);
            weaponDef->accuracyGraphKnotCount[weaponType] = accuracyGraphKnotCount;
            weaponDef->originalAccuracyGraphKnotCount[weaponType] = weaponDef->accuracyGraphKnotCount[weaponType];
        }
    }
    return 1;
}

WeaponDef *BG_LoadDefaultWeaponDef_LoadObj()
{
    InitWeaponDef(&bg_defaultWeaponDefs);
    bg_defaultWeaponDefs.szInternalName = "none";
    bg_defaultWeaponDefs.accuracyGraphName[0] = "noweapon.accu";
    bg_defaultWeaponDefs.accuracyGraphName[1] = "noweapon.accu";
    bg_defaultWeaponDefs.sprintDurationScale = 1.75;
    G_ParseWeaponAccurayGraphs(&bg_defaultWeaponDefs);
    return &bg_defaultWeaponDefs;
}

WeaponDef *BG_LoadDefaultWeaponDef()
{
    if (IsFastFileLoad())
        return BG_LoadDefaultWeaponDef_FastFile();
    else
        return BG_LoadDefaultWeaponDef_LoadObj();
}

WeaponDef *BG_LoadDefaultWeaponDef_FastFile()
{
    return DB_FindXAssetHeader(ASSET_TYPE_WEAPON, "none").weapon;
}

int Weapon_GetStringArrayIndex(const char *value, char **stringArray, int arraySize)
{
    int arrayIndex; // [esp+0h] [ebp-4h]

    iassert(value);
    iassert(stringArray);

    for (arrayIndex = 0; arrayIndex < arraySize; ++arrayIndex)
    {
        if (!I_stricmp(value, stringArray[arrayIndex]))
            return arrayIndex;
    }
    return -1;
}

snd_alias_list_t **BG_RegisterSurfaceTypeSounds(const char *surfaceSoundBase)
{
    char *v2; // eax
    snd_alias_list_t *SoundAlias; // eax
    char v4; // [esp+3h] [ebp-131h]
    char *v5; // [esp+8h] [ebp-12Ch]
    const char *v6; // [esp+Ch] [ebp-128h]
    snd_alias_list_t **result; // [esp+20h] [ebp-114h]
    char aliasName[260]; // [esp+24h] [ebp-110h] BYREF
    snd_alias_list_t *defaultAliasList; // [esp+12Ch] [ebp-8h]
    int i; // [esp+130h] [ebp-4h]

    iassert(surfaceSoundBase);

    if (!*surfaceSoundBase)
        return 0;

    for (i = 0; i < surfaceTypeSoundListCount; ++i)
    {
        if (!I_strcmp(surfaceTypeSoundLists[i].surfaceSoundBase, surfaceSoundBase))
            return surfaceTypeSoundLists[i].soundAliasList;
    }
    if (surfaceTypeSoundListCount == 16)
        Com_Error(ERR_DROP, "Exceeded MAX_SURFACE_TYPE_SOUND_LISTS (%d)", 16);

    result = (snd_alias_list_t **)Hunk_AllocLow(sizeof(snd_alias_list_t *) * SURF_TYPECOUNT, "BG_RegisterSurfaceTypeSounds", 15);
    Com_sprintf(aliasName, 0x100u, "%s_default", surfaceSoundBase);
    defaultAliasList = Com_FindSoundAlias(aliasName);
    for (i = 0; i < 29; ++i)
    {
        v2 = (char*)Com_SurfaceTypeToName(i);
        Com_sprintf(aliasName, 0x100u, "%s_%s", surfaceSoundBase, v2);
        SoundAlias = Com_FindSoundAlias(aliasName);
        result[i] = SoundAlias;
        if (!result[i])
            result[i] = defaultAliasList;
    }
    surfaceTypeSoundLists[surfaceTypeSoundListCount].surfaceSoundBase = (char *)Hunk_AllocLow(
        strlen(surfaceSoundBase) + 1,
        "BG_RegisterSurfaceTypeSounds",
        15);
    v6 = surfaceSoundBase;
    v5 = surfaceTypeSoundLists[surfaceTypeSoundListCount].surfaceSoundBase;
    do
    {
        v4 = *v6;
        *v5++ = *v6++;
    } while (v4);
    surfaceTypeSoundLists[surfaceTypeSoundListCount++].soundAliasList = result;
    return result;
}

int BG_ParseWeaponDefSpecificFieldType(uint8_t *pStruct, const char *pValue, int iFieldType)
{
    uint16_t LowercaseString_DONE; // ax
    uint16_t v5; // ax
    int result; // eax
    char v7; // [esp+3h] [ebp-91h]
    char *v8; // [esp+8h] [ebp-8Ch]
    const char *v9; // [esp+Ch] [ebp-88h]
    int v10; // [esp+10h] [ebp-84h]
    const char *pos; // [esp+38h] [ebp-5Ch] BYREF
    int numHideTags; // [esp+3Ch] [ebp-58h]
    int numNoteTrackMappings; // [esp+40h] [ebp-54h]
    char keyName[64]; // [esp+44h] [ebp-50h] BYREF
    int arrayIndex; // [esp+88h] [ebp-Ch]
    const char *token; // [esp+8Ch] [ebp-8h]
    WeaponDef *weapDef; // [esp+90h] [ebp-4h]

    iassert(pStruct);
    iassert(pValue);

    weapDef = (WeaponDef *)pStruct;
    switch (iFieldType)
    {
    case WFT_WEAPONTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)szWeapTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon type %s in %s", pValue, weapDef->szInternalName);
        weapDef->weapType = (weapType_t)arrayIndex;
        goto LABEL_86;
    case WFT_WEAPONCLASS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)szWeapClassNames, 10);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon class %s in %s", pValue, weapDef->szInternalName);
        weapDef->weapClass = (weapClass_t)arrayIndex;
        goto LABEL_86;
    case WFT_OVERLAYRETICLE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapOverlayReticleNames, 2);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon reticle %s in %s", pValue, weapDef->szInternalName);
        weapDef->overlayReticle = (weapOverlayReticle_t)arrayIndex;
        goto LABEL_86;
    case WFT_PENETRATE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)penetrateTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon penetrate type %s in %s", pValue, weapDef->szInternalName);
        weapDef->penetrateType = (PenetrateType)arrayIndex;
        goto LABEL_86;
    case WFT_IMPACT_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)impactTypeNames, 9);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon impact type %s in %s", pValue, weapDef->szInternalName);
        weapDef->impactType = (ImpactType)arrayIndex;
        goto LABEL_86;
    case WFT_STANCE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapStanceNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon stance %s in %s", pValue, weapDef->szInternalName);
        weapDef->stance = (weapStance_t)arrayIndex;
        goto LABEL_86;
    case WFT_PROJ_EXPLOSION:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szProjectileExplosionNames, 7);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon projExplosion %s in %s", pValue, weapDef->szInternalName);
        weapDef->projExplosion = (weapProjExposion_t)arrayIndex;
        goto LABEL_86;
    case WFT_OFFHAND_CLASS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)offhandClassNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon offhand class %s in %s", pValue, weapDef->szInternalName);
        weapDef->offhandClass = (OffhandClass)arrayIndex;
        goto LABEL_86;
    case WFT_ANIMTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, g_playerAnimTypeNames, g_playerAnimTypeNamesCount);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon player anim type %s in %s", pValue, weapDef->szInternalName);
        weapDef->playerAnimType = arrayIndex;
        goto LABEL_86;
    case WFT_ACTIVE_RETICLE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)activeReticleNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon active reticle type %s in %s", pValue, weapDef->szInternalName);
        weapDef->activeReticleType = (activeReticleType_t)arrayIndex;
        goto LABEL_86;
    case WFT_GUIDED_MISSILE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)guidedMissileNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon guided missile type %s in %s", pValue, weapDef->szInternalName);
        weapDef->guidedMissileType = (guidedMissileType_t)arrayIndex;
        goto LABEL_86;
    case WFT_BOUNCE_SOUND:
        weapDef->bounceSound = BG_RegisterSurfaceTypeSounds(pValue);
        goto LABEL_86;
    case WFT_STICKINESS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)stickinessNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon stickiness %s in %s", pValue, weapDef->szInternalName);
        weapDef->stickiness = (WeapStickinessType)arrayIndex;
        goto LABEL_86;
    case WFT_OVERLAYINTERFACE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)overlayInterfaceNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon overlay interface %s in %s", pValue, weapDef->szInternalName);
        weapDef->overlayInterface = (WeapOverlayInteface_t)arrayIndex;
        goto LABEL_86;
    case WFT_INVENTORYTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapInventoryTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon inventory type %s in %s", pValue, weapDef->szInternalName);
        weapDef->inventoryType = (weapInventoryType_t)arrayIndex;
        goto LABEL_86;
    case WFT_FIRETYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapFireTypeNames, 5);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon firetype %s in %s", pValue, weapDef->szInternalName);
        weapDef->fireType = (weapFireType_t)arrayIndex;
        goto LABEL_86;
    case WFT_AMMOCOUNTER_CLIPTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)ammoCounterClipNames, 7);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon ammo counter clip %s in %s", pValue, weapDef->szInternalName);
        weapDef->ammoCounterClip = (ammoCounterClipType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_HUD:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon hud icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->hudIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_AMMOCOUNTER:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon ammo counter icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->ammoCounterIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_KILL:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon kill icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->killIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_DPAD:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon dpad icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->dpadIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_HIDETAGS:
        numHideTags = 0;
        pos = pValue;
        while (1)
        {
            token = (const char *)Com_Parse(&pos);
            if (!pos)
                break;
            if (numHideTags >= 8)
                Com_Error(ERR_DROP, "maximum hide tags (%s) exceeded: %i > %i'", token, numHideTags, 8);
            weapDef->hideTags[numHideTags] = SL_GetStringOfSize((char *)token, 0, strlen(token) + 1, MT_TYPE_MODEL_PART);
            weapDef->hideTags[numHideTags] = SL_ConvertToLowercase(weapDef->hideTags[numHideTags], 0, MT_TYPE_MODEL_PART);
            ++numHideTags;
        }
        goto LABEL_86;
    case WFT_NOTETRACKSOUNDMAP:
        numNoteTrackMappings = 0;
        pos = pValue;
        keyName[0] = 0;
        while (1)
        {
            token = (const char *)Com_Parse(&pos);
            if (!pos)
                break;
            if (numNoteTrackMappings >= 16)
                Com_Error(ERR_DROP, "Max notetrack-to-sound mappings (%i) exceeded with entry '%s'", 16, token);
            if (keyName[0])
            {
                LowercaseString_DONE = SL_GetLowercaseString(keyName, 0);
                weapDef->notetrackSoundMapKeys[numNoteTrackMappings] = LowercaseString_DONE;
                v5 = SL_GetLowercaseString(token, 0);
                weapDef->notetrackSoundMapValues[numNoteTrackMappings++] = v5;
                keyName[0] = 0;
            }   
            else
            {
                v10 = strlen(token);
                if (v10 >= 63)
                    Com_Error(ERR_DROP, "Notetrack - to - sound: keyname \"%s\" is too long(length % i / % i).", token, v10, 63);
                v9 = token;
                v8 = keyName;
                do
                {
                    v7 = *v9;
                    *v8++ = *v9++;
                } while (v7);
            }
        }
        if (keyName[0])
            Com_PrintWarning(
                CON_CHANNEL_DONT_FILTER,
                "Notetrack-to-Sound: Weapon '%s' has bad entry; notetrack '%s' doesn't have a corresponding sound.\n",
                weapDef->szInternalName,
                keyName);
    LABEL_86:
        result = 1;
        break;
    default:
        Com_Error(ERR_DROP, "Bad field type %i in %s", iFieldType, weapDef->szInternalName);
        result = 0;
        break;
    }
    return result;
}

void BG_SetupTransitionTimes(WeaponDef *weapDef)
{
    double v1; // st7
    double v2; // st7

    if (weapDef->iAdsTransInTime <= 0)
        v1 = 1.0 / (float)300.0;
    else
        v1 = 1.0 / (double)weapDef->iAdsTransInTime;
    weapDef->fOOPosAnimLength[0] = v1;
    if (weapDef->iAdsTransOutTime <= 0)
        v2 = 1.0 / (float)500.0;
    else
        v2 = 1.0 / (double)weapDef->iAdsTransOutTime;
    weapDef->fOOPosAnimLength[1] = v2;
}

void BG_CheckWeaponDamageRanges(WeaponDef *weapDef)
{
    if (weapDef->fMaxDamageRange <= 0.0f)
        weapDef->fMaxDamageRange = 999999.0f;
    if (weapDef->fMinDamageRange <= 0.0f)
        weapDef->fMinDamageRange = 999999.12f;
}

void BG_CheckProjectileValues(WeaponDef *weaponDef)
{
    iassert(weaponDef->weapType == WEAPTYPE_PROJECTILE);

    if ((double)weaponDef->iProjectileSpeed <= 0.0)
        Com_Error(ERR_DROP, "Projectile speed for WeapType %s must be greater than 0.0", weaponDef->szDisplayName);

    if (weaponDef->destabilizationCurvatureMax >= 1000000000.0f || weaponDef->destabilizationCurvatureMax < 0.0)
        Com_Error(
            ERR_DROP,
            "Destabilization angle for for WeapType %s must be between 0 and 45 degrees",
            weaponDef->szDisplayName);

    if (weaponDef->destabilizationRateTime < 0.0)
        Com_Error(ERR_DROP, "Destabilization rate time for for WeapType %s must be non-negative", weaponDef->szDisplayName);
}

WeaponDef *BG_LoadWeaponDefInternal(const char *one, const char *two)
{
    snd_alias_list_t *SoundAlias; // eax
    snd_alias_list_t *v4; // eax
    snd_alias_list_t *v5; // eax
    snd_alias_list_t *v6; // eax
    snd_alias_list_t *v7; // eax
    char buffer[10244]; // [esp+1Ch] [ebp-2858h] BYREF
    int f; // [esp+2820h] [ebp-54h] BYREF
    int len; // [esp+2824h] [ebp-50h]
    signed int v11; // [esp+2828h] [ebp-4Ch]
    char dest[64]; // [esp+282Ch] [ebp-48h] BYREF
    WeaponDef *weapDef; // [esp+2870h] [ebp-4h]

    len = strlen("WEAPONFILE");
    weapDef = (WeaponDef *)Hunk_AllocLow(sizeof(WeaponDef), "BG_LoadWeaponDefInternal", 9);
    InitWeaponDef(weapDef);
    Com_sprintf(dest, 0x40u, "weapons/%s/%s", one, two);
    v11 = FS_FOpenFileByMode(dest, &f, FS_READ);
    if (v11 >= 0)
    {
        FS_Read((uint8_t *)buffer, len, f);
        buffer[len] = 0;
        if (!strncmp(buffer, "WEAPONFILE", len))
        {
            if ((uint32_t)(v11 - len) < 0x2800)
            {
                memset((uint8_t *)buffer, 0, 0x2800u);
                FS_Read((uint8_t *)buffer, v11 - len, f);
                buffer[v11 - len] = 0;
                FS_FCloseFile(f);
                if (Info_Validate(buffer))
                {
                    SetConfigString((char **)weapDef, two);
                    if (ParseConfigStringToStructCustomSize(
                        (uint8_t *)weapDef,
                        weaponDefFields,
                        502,
                        buffer,
                        WFT_NUM_FIELD_TYPES,
                        BG_ParseWeaponDefSpecificFieldType,
                        SetConfigString2))
                    {
                        if (I_stricmp(two, "defaultweapon_mp"))
                        {
                            if (!weapDef->viewLastShotEjectEffect)
                                weapDef->viewLastShotEjectEffect = weapDef->viewShellEjectEffect;
                            if (!weapDef->worldLastShotEjectEffect)
                                weapDef->worldLastShotEjectEffect = weapDef->worldShellEjectEffect;
                            if (!weapDef->raiseSound)
                            {
                                SoundAlias = Com_FindSoundAlias("weap_raise");
                                weapDef->raiseSound = SoundAlias;
                            }
                            if (!weapDef->putawaySound)
                            {
                                v4 = Com_FindSoundAlias("weap_putaway");
                                weapDef->putawaySound = v4;
                            }
                            if (!weapDef->pickupSound)
                            {
                                v5 = Com_FindSoundAlias("weap_pickup");
                                weapDef->pickupSound = v5;
                            }
                            if (!weapDef->ammoPickupSound)
                            {
                                v6 = Com_FindSoundAlias("weap_ammo_pickup");
                                weapDef->ammoPickupSound = v6;
                            }
                            if (!weapDef->emptyFireSound)
                            {
                                v7 = Com_FindSoundAlias("weap_dryfire_smg_npc");
                                weapDef->emptyFireSound = v7;
                            }
                        }
                        BG_SetupTransitionTimes(weapDef);
                        BG_CheckWeaponDamageRanges(weapDef);
                        if (weapDef->enemyCrosshairRange > 15000.0)
                            Com_Error(ERR_DROP, "Enemy crosshair ranges should be less than %f ", 15000.0);
                        if (weapDef->weapType == WEAPTYPE_PROJECTILE)
                            BG_CheckProjectileValues(weapDef);
                        if (G_ParseWeaponAccurayGraphs(weapDef))
                        {
                            I_strlwr((char *)weapDef->szAmmoName);
                            I_strlwr((char *)weapDef->szClipName);
                            return weapDef;
                        }
                        else
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: \"%s\" is not a valid weapon file\n", dest);
                    return 0;
                }
            }
            else
            {
                Com_PrintWarning(
                    CON_CHANNEL_PLAYERWEAP,
                    "WARNING: \"%s\" Is too long of a weapon file to parse (fileLength = %d identifierLength = %d)\n",
                    dest,
                    v11,
                    len);
                FS_FCloseFile(f);
                return 0;
            }
        }
        else
        {
            Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: \"%s\" does not appear to be a weapon file\n", dest);
            FS_FCloseFile(f);
            return 0;
        }
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: Could not load weapon file '%s'\n", dest);
        return 0;
    }
}


WeaponDef *BG_LoadWeaponDef_LoadObj(const char *name)
{
    WeaponDef *weapDef; // [esp+0h] [ebp-4h]

    if (!*name)
        return 0;
#ifdef KISAK_MP
    weapDef = BG_LoadWeaponDefInternal("mp", name);
#elif KISAK_SP
    weapDef = BG_LoadWeaponDefInternal("sp", name);
#endif
    if (weapDef)
        return weapDef;

#ifdef KISAK_MP
    weapDef = BG_LoadWeaponDefInternal("mp", "defaultweapon_mp");
#elif KISAK_SP
    weapDef = BG_LoadWeaponDefInternal("sp", "defaultweapon");
#endif

    if (!weapDef)
        Com_Error(ERR_DROP, "BG_LoadWeaponDef: Could not find default weapon");

    SetConfigString((char **)weapDef, name);
    return weapDef;
}
