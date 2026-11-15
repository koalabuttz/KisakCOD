// KisakCOD's own bot brain for multiplayer test clients (docs/bots.md).
//
// Each server frame SV_UpdateBots asks Bot_UserMove for every active test
// client. With bot_brain 1 the brain builds the bot's user command:
//
//   read      the bot's own player state (view, weapon, alive or dead), as a
//             player's client knows it;
//   sense     ten times a second, what its eyes and ears get of the other
//             players (bot_senses.cpp) goes into what it knows
//             (bot_perception.h): noticed, remembered and heard players;
//   decide    what to do (bot_mind.h: roam, hunt, fight, take cover), where
//             to go for it on the map's navigation mesh, and what to look
//             at: the player it sees, a sound it just heard, or the corner of
//             its way an enemy would come round (pre-aim); fight the player
//             it sees (bot_combat.h, with bot_combat 1); ask to respawn when
//             dead;
//   team      call out the enemies it sees to its team, and play the part
//             the team's commander gave it (bot_team.h): push to where the
//             team saw enemies, anchor a spot that overlooks them, roam, or
//             attack and defend the objectives on its compass;
//   move      the view motor (bot_motor.h) turns toward that at a human pace,
//             the hand of bot_combat.h aims, and the bot kicks its own view
//             when it fires, as a player's client does;
//   send      the usercmd_s goes through SV_ClientThink like a player's.
//
// The brain reads other players only through the sense step.

#include <universal/q_shared.h>
#include <server_mp/server_mp.h>
#include <server/sv_game.h>
#include <game_mp/g_main_mp.h>
#include <bgame/bg_local.h>
#include <bgame/bg_public.h>

#include "bot_brain.h"
#include "bot_combat.h"
#include "bot_grenades.h"
#include "bot_mind.h"
#include "bot_motor.h"
#include "bot_navmesh.h"
#include "bot_perception.h"
#include "bot_team.h"

#include <cmath>
#include <cstring>

namespace
{

namespace kb = kisak::bots;

const dvar_t *bot_brain;
const dvar_t *bot_log;
const dvar_t *bot_combat;

constexpr int kMaxBotSlots = 64;

struct BotSlot
{
    // The bot's address port: SV_AddTestClient gives every bot a new one, so
    // a slot whose port changed holds a new bot and starts afresh.
    uint32_t port = 0;
    bool active = false;
    int lastTime = 0;
    kb::Rng rng;
    kb::ViewMotorState view;
    // The view this bot last asked for, to notice when the server moved it
    // (a spawn, a killcam, a script's setPlayerAngles).
    float sentView[2] = {0.0f, 0.0f};
    kb::IdleLook look;
    float lookHeld = 0.0f;
    int respawnTap = 0;

    // Walking to a place: the path's corners, the next one, and when the bot
    // last got closer to it.
    static constexpr int kMaxCorners = 48;
    float path[kMaxCorners][3];
    int pathCount = 0;
    int pathIndex = 0;
    float bestDistance = 0.0f;
    int progressTime = 0;
    int nextPlanTime = 0;
    bool jumpHeld = false;
    int nextJump = 0;
    int stallJumps = 0; // jumps since it last got closer to the corner
    int stuckCount = 0;
    float lastOrigin[3] = {0.0f, 0.0f, 0.0f};

    // What the bot knows of the other players, and when it last sensed.
    kb::Perception perception;
    int lastSense = 0;
    int nextSense = 0;

    // Fighting: the hand's aim, the trigger, the weapon's kick on the view
    // (the bot's own, as a player's client keeps it), moving under fire.
    kb::AimState aim;
    kb::TriggerState trigger;
    // When its last fight ended (or changed player), and the shots it has kept
    // going: since when, and the last one.
    int aimEndedAt = -100000;
    int sprayStart = 0;
    int lastShotAt = -100000;
    int nextFastLog = 0;
    kb::ViewKick kick;
    kb::StrafeState strafe;
    int eventSequence = 0;
    bool eventKnown = false;
    bool adsErrorDone = false;
    bool firstShotLogged = false;
    int lastFightTime = -100000;
    bool reloadHeld = false;
    int nextReloadTime = 0;

    // Deciding: what it is doing, its leanings, where it has been and met
    // enemies, where it is going and why.
    kb::Mind mind;
    kb::Personality personality;
    kb::AreaMemory areas;
    float goal[3] = {0.0f, 0.0f, 0.0f};
    bool hasGoal = false;
    kb::Intent goalFor = kb::Intent::Roam;
    int holdUntil = 0;
    bool holdCrouched = false;
    // Lies prone while it holds (on a flag it takes, the patient most often).
    bool holdProne = false;
    // Cover from the threat it knows of, looked for at most once a second.
    float cover[3] = {0.0f, 0.0f, 0.0f};
    bool coverKnown = false;
    int nextCoverSearch = 0;
    // The corner of its way it pre-aims, looked for four times a second.
    float preAim[3] = {0.0f, 0.0f, 0.0f};
    bool preAimKnown = false;
    int nextPreAim = 0;
    // The HUD's hit indicator: when it was last hit, and from which way.
    int damageEvent = 0;
    bool damageKnown = false;
    int hurtAt = -1000000;
    bool hurtFromKnown = false;
    float hurtFrom[3] = {0.0f, 0.0f, 0.0f};
    // A lead it found no way to (a sound on a floor its way does not reach):
    // left alone until then.
    float noWayTo[3] = {0.0f, 0.0f, 0.0f};
    int noWayUntil = 0;

    // Its team (bot_team.h): its part, the objective that part is about, when
    // it last called out each enemy, and the goal the part gave it.
    kb::Role role = kb::Role::Roam;
    bool objectiveKnown = false;
    float objective[3] = {0.0f, 0.0f, 0.0f};
    int calledAt[kMaxBotSlots] = {};
    bool roleGoal = false;
    int roleUntil = 0;
    int pushedCall = -1;
    float holdFacing[3] = {0.0f, 0.0f, 0.0f};
    bool holdFacingKnown = false;
    bool atObjective = false;
    int roleRetryAt = 0;
    bool shortLogged = false; // said it stopped short of its objective
    int onObjectiveSince = 0; // when an attacker stepped onto its objective, or 0

    // A grenade: lining up the throw, holding the button (the fuse runs),
    // when it may throw again, and the enemy it last thought about one for.
    int grenadePhase = 0; // 0 none, 1 lining up, 2 holding
    float grenadeView[2] = {0.0f, 0.0f};
    int grenadeUntil = 0;
    int nextGrenade = 0;
    int grenadeThoughtAt = -1;
    float grenadeDistance = 0.0f;
    // The knife: pressed on one frame, released on the next.
    bool meleeHeld = false;
    int nextMelee = 0;
};

BotSlot s_slots[kMaxBotSlots];

// A team of bots: what they said to each other, and the objectives on their
// compass, sorted for the commander.
struct TeamState
{
    kb::TeamBoard board;
    int lastTime = 0;
    int nextCommand = 0;
    static constexpr int kMaxObjectives = MAX_OBJECTIVES;
    float take[kMaxObjectives][3];
    float hold[kMaxObjectives][3];
    int takeCount = 0;
    int holdCount = 0;
    // Where the team started: its side of the map.
    float side[3] = {0.0f, 0.0f, 0.0f};
    bool sideKnown = false;
};

TeamState s_teams[TEAM_NUM_TEAMS];
int s_commandFrame = -1;

kb::ViewMotorParams IdleMotor()
{
    kb::ViewMotorParams params;
    params.responsiveness = 5.0f;
    params.maxSpeed = 120.0f;
    return params;
}

// Watching a player it noticed, remembers or heard: quicker than a look
// around, no faster than walking (aiming comes with combat, phase 3).
kb::ViewMotorParams WatchMotor()
{
    kb::ViewMotorParams params;
    params.responsiveness = 10.0f;
    params.maxSpeed = 200.0f;
    return params;
}

const kb::PerceptionParams &Senses()
{
    static const kb::PerceptionParams params;
    return params;
}

constexpr int kSenseMs = 100;

const kb::SkillProfile &Skill()
{
    static const kb::SkillProfile skill;
    return skill;
}

bool CombatOn()
{
    return bot_combat && bot_combat->current.integer != 0;
}

bool LogOn()
{
    return bot_log && bot_log->current.integer;
}

kb::ViewMotorParams WalkMotor()
{
    kb::ViewMotorParams params;
    params.responsiveness = 7.0f;
    params.maxSpeed = 200.0f;
    return params;
}

bool IsDead(const playerState_s *ps)
{
    return ps->pm_type == PM_DEAD || ps->pm_type == PM_DEAD_LINKED;
}

float NextUnit(void *rng)
{
    return static_cast<kb::Rng *>(rng)->Unit();
}

float Distance2D(const float *a, const float *b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

// How close counts as being at a corner, and how long a bot may make no
// headway toward one before it jumps, then before it gives up on the way.
constexpr float kCornerReach = 20.0f;
constexpr int kJumpAfterMs = 1200;
constexpr int kReplanAfterMs = 2500;

// The view (pitch, yaw) from the bot's eye toward `place`.
void ViewToward(const playerState_s *ps, const float *place, float *view)
{
    const float dx = place[0] - ps->origin[0];
    const float dy = place[1] - ps->origin[1];
    const float dz = place[2] - (ps->origin[2] + ps->viewHeightCurrent);
    view[0] = -std::atan2(dz, std::hypot(dx, dy)) * (180.0f / 3.14159265358979f);
    view[1] = std::atan2(dy, dx) * (180.0f / 3.14159265358979f);
}

// A sense tick when one is due; logs what the bot noticed when bot_log is set.
void Sense(BotSlot &bot, int clientNum)
{
    if (svs.time < bot.nextSense)
        return;
    // The first tick, and the first after a gap (dead, spectating), counts as
    // one tick's worth of looking.
    const bool recent = bot.lastSense && svs.time - bot.lastSense <= 2 * kSenseMs;
    const int lastSense = recent ? bot.lastSense : svs.time - kSenseMs;
    BotNotice notices[8];
    const int count = Bot_Sense(clientNum, bot.perception, Senses(), svs.time, lastSense, bot.rng, notices, 8);
    bot.lastSense = svs.time;
    bot.nextSense = svs.time + kSenseMs;
    if (!bot_log || !bot_log->current.integer)
        return;
    for (int i = 0; i < count; ++i)
        Com_Printf(CON_CHANNEL_SERVER,
                   "bots: %d noticed %d after %d ms in view, %.0f units, %.0f degrees off centre, %.0f%% showing\n",
                   clientNum, notices[i].entity, notices[i].afterMs, notices[i].distance, notices[i].offDegrees,
                   notices[i].exposure * 100.0f);
}

// The way from `origin` to `goal`. False when there is none.
bool PlanWalk(BotSlot &bot, const kb::NavMesh &nav, const float *origin, const float *goal)
{
    bot.pathCount = 0;
    bot.pathIndex = 0;
    bot.preAimKnown = false;
    bot.nextPreAim = 0;
    const int count = nav.FindPath(origin, goal, bot.path, BotSlot::kMaxCorners);
    if (count < 2)
        return false;
    bot.pathCount = count;
    bot.pathIndex = 1; // corner 0 is where the bot stands
    bot.bestDistance = Distance2D(origin, bot.path[1]);
    bot.progressTime = svs.time;
    bot.stallJumps = 0;
    return true;
}

// One server frame of walking the way planned: turn toward the next corner
// (or look at `watch`), move toward it, jump when stuck, drop the way when
// still stuck. Returns false when the bot has no way or has arrived. With
// `aiming` the view is the fight's and is left alone.
bool Walk(BotSlot &bot, const playerState_s *ps, float seconds, usercmd_s &cmd, const float *watch,
          bool aiming = false)
{
    const float *origin = ps->origin;
    if (bot.pathIndex >= bot.pathCount)
        return false;

    // The last corner of the way to an objective it takes is the objective
    // itself, often a flag on a base the walls of the map leave out: close
    // to it, on the base or against it, is there.
    auto reached = [&](int index) {
        const bool objective = bot.roleGoal && bot.role == kb::Role::Attack && index == bot.pathCount - 1;
        return Distance2D(origin, bot.path[index]) < (objective ? 64.0f : kCornerReach)
               && std::fabs(origin[2] - bot.path[index][2]) < (objective ? 72.0f : 48.0f);
    };
    while (bot.pathIndex < bot.pathCount && reached(bot.pathIndex))
    {
        ++bot.pathIndex;
        if (bot.pathIndex < bot.pathCount)
        {
            bot.bestDistance = Distance2D(origin, bot.path[bot.pathIndex]);
            bot.progressTime = svs.time;
        }
    }
    if (bot.pathIndex >= bot.pathCount)
    {
        bot.stuckCount = 0;
        return false;
    }

    const float *corner = bot.path[bot.pathIndex];
    const float distance = Distance2D(origin, corner);
    if (distance < bot.bestDistance - 8.0f)
    {
        bot.bestDistance = distance;
        bot.progressTime = svs.time;
        bot.stallJumps = 0;
    }
    const int stalled = svs.time - bot.progressTime;
    if (stalled > kReplanAfterMs)
    {
        ++bot.stuckCount;
        bot.pathCount = 0;
        bot.pathIndex = 0;
        // Stuck twice on the way to the same place: go somewhere else.
        if (bot.stuckCount >= 2)
        {
            bot.hasGoal = false;
            bot.stuckCount = 0;
            if (bot.roleGoal)
            {
                bot.roleGoal = false;
                bot.roleRetryAt = svs.time + 3000;
                if (bot.role == kb::Role::Attack && LogOn())
                    Com_Printf(CON_CHANNEL_SERVER, "bots: %d is stuck on its way to an objective at %.0f %.0f %.0f\n",
                               static_cast<int>(&bot - s_slots), origin[0], origin[1], origin[2]);
            }
        }
        return false;
    }

    if (aiming)
        ;
    else if (watch)
        kb::StepView(bot.view, watch[0], watch[1], seconds, WatchMotor());
    else
        kb::StepView(bot.view, 0.0f, kb::YawToward(origin, corner), seconds, WalkMotor());
    const kb::MoveKeys keys = kb::MoveToward(bot.view.angles[1], origin, corner);
    cmd.forwardmove = static_cast<char>(keys.forward);
    cmd.rightmove = static_cast<char>(keys.right);

    // Jump is pressed on one frame and released on the next: the game jumps
    // on the press only. Once a second at most: a jump into a ledge mantles
    // (the weapon goes down meanwhile), and PC bot brain run 11 had a bot
    // that pressed it every other frame keep its gun down for 28 s.
    // Two jumps that got it no closer: no more until it does.
    if (stalled > kJumpAfterMs && !bot.jumpHeld && svs.time >= bot.nextJump && bot.stallJumps < 2)
    {
        cmd.buttons |= BUTTON_JUMP;
        bot.jumpHeld = true;
        bot.nextJump = svs.time + 1000;
        ++bot.stallJumps;
    }
    else
    {
        bot.jumpHeld = false;
    }
    return true;
}

// The weapon in the bot's hands, or null for none.
const WeaponDef *HeldWeapon(const playerState_s *ps)
{
    if (!ps->weapon || !BG_ValidateWeaponNumber(ps->weapon))
        return nullptr;
    return BG_GetWeaponDef(ps->weapon);
}

// A gun the bot fights with: bullets, not a grenade, a launcher, an item or
// a turret.
bool IsGun(const WeaponDef *def)
{
    return def && def->weapType == WEAPTYPE_BULLET && def->offhandClass == OFFHAND_CLASS_NONE
           && def->weapClass != WEAPCLASS_ITEM && def->weapClass != WEAPCLASS_TURRET
           && def->weapClass != WEAPCLASS_NON_PLAYER && def->weapClass != WEAPCLASS_GRENADE;
}

// The view kick of the bot's own shots since the last frame, then `ms` of it
// settling: what CG_KickAngles and the fire events do on a player's client,
// with the weapon's own numbers (BG_WeaponFireRecoil).
void KickView(BotSlot &bot, const playerState_s *ps, int ms)
{
    if (!bot.eventKnown)
    {
        bot.eventSequence = ps->eventSequence;
        bot.eventKnown = true;
    }
    const uint32_t weapon = BG_GetViewmodelWeaponIndex(ps);
    const WeaponDef *def = weapon && BG_ValidateWeaponNumber(weapon) ? BG_GetWeaponDef(weapon) : nullptr;
    int from = bot.eventSequence;
    if (ps->eventSequence - from > 4)
        from = ps->eventSequence - 4;
    for (int sequence = from; sequence < ps->eventSequence; ++sequence)
    {
        const int event = ps->events[sequence & 3] & 0xff;
        if (event != EV_FIRE_WEAPON && event != EV_FIRE_WEAPON_LASTSHOT && event != EV_FIRE_WEAPON_MG42)
            continue;
        if (!def)
            continue;
        const bool aimed = ps->fWeaponPosFrac == 1.0f;
        kb::KickParams params;
        params.pitchMin = aimed ? def->fAdsViewKickPitchMin : def->fHipViewKickPitchMin;
        params.pitchMax = aimed ? def->fAdsViewKickPitchMax : def->fHipViewKickPitchMax;
        params.yawMin = aimed ? def->fAdsViewKickYawMin : def->fHipViewKickYawMin;
        params.yawMax = aimed ? def->fAdsViewKickYawMax : def->fHipViewKickYawMax;
        float reduce = 1.0f;
        if (ps->weaponRestrictKickTime > 0)
            reduce = (aimed ? def->adsGunKickReducedKickPercent : def->hipGunKickReducedKickPercent) * 0.01f;
        const float unitPitch = bot.rng.Unit();
        const float unitYaw = bot.rng.Unit();
        kb::KickOnShot(bot.kick, params, reduce, unitPitch, unitYaw);
    }
    bot.eventSequence = ps->eventSequence;

    float centerSpeed = 2400.0f;
    if (def)
        centerSpeed = ps->fWeaponPosFrac <= 0.5f ? def->fHipViewKickCenterSpeed : def->fAdsViewKickCenterSpeed;
    kb::StepKick(bot.kick, centerSpeed, ms);
}

// Another gun the bot carries with ammunition left, 0 for none.
int OtherLoadedGun(const playerState_s *ps)
{
    const uint32_t count = BG_GetNumWeapons();
    for (uint32_t weapon = 1; weapon < count; ++weapon)
    {
        if (weapon == static_cast<uint32_t>(ps->weapon) || !BG_IsWeaponValid(ps, weapon))
            continue;
        if (IsGun(BG_GetWeaponDef(weapon)) && BG_WeaponAmmo(ps, weapon) > 0)
            return static_cast<int>(weapon);
    }
    return 0;
}

// Reloads when the clip is empty, or when it is under half and the bot has
// not fought for a couple of seconds; switches guns when this one is dry.
void LookAfterWeapon(BotSlot &bot, const playerState_s *ps, usercmd_s &cmd)
{
    const WeaponDef *def = HeldWeapon(ps);
    bool press = false;
    if (IsGun(def) && def->iClipSize > 0)
    {
        if (BG_WeaponAmmo(ps, ps->weapon) <= 0)
        {
            const int other = OtherLoadedGun(ps);
            if (other)
                cmd.weapon = static_cast<uint8_t>(other);
        }
        const int clip = ps->ammoclip[BG_ClipForWeapon(ps->weapon)];
        const int reserve = ps->ammo[BG_AmmoForWeapon(ps->weapon)];
        const bool calm = svs.time - bot.lastFightTime > 2000;
        press = reserve > 0 && ps->weaponstate == WEAPON_READY
                && (clip == 0 || (calm && clip * 2 < def->iClipSize));
    }
    // A press is one frame down, then up; again at most twice a second.
    if (press && !bot.reloadHeld && svs.time >= bot.nextReloadTime)
    {
        cmd.buttons |= BUTTON_RELOAD;
        bot.reloadHeld = true;
        bot.nextReloadTime = svs.time + 500;
    }
    else
    {
        bot.reloadHeld = false;
    }
}

// One frame of fighting the player the bot focuses on: aim the hand at where
// it thinks they are, and, while it sees them, the sight, the trigger and a
// strafe. Returns the strafe (-1, 0 to keep walking, 1).
int Fight(BotSlot &bot, int clientNum, const playerState_s *ps, const kb::Focus &focus, float seconds,
          usercmd_s &cmd)
{
    const kb::SkillProfile &skill = Skill();
    const kb::Contact &contact = bot.perception.contacts[focus.entity];
    if (bot.aim.target != focus.entity)
    {
        // Straight from another fight: the eye and hand still have the last
        // player, and need a moment to take in the next.
        const bool switching = bot.aim.target >= 0 || svs.time - bot.aimEndedAt < skill.switchWindowMs;
        kb::Engage(bot.aim, focus.entity, contact.noticedAt, svs.time, skill, bot.rng, switching);
        bot.trigger = kb::TriggerState();
        bot.firstShotLogged = false;
    }

    float place[3];
    kb::LeadPlace(contact.lastSeen, contact.velocity, svs.time - contact.lastSeenAt, skill, place);
    place[2] += bot.aim.heightOffset;
    float target[2];
    ViewToward(ps, place, target);
    // A spray kept going (shots no more than 0.3 s apart) wanders; clicks of a
    // semi-automatic half as much.
    int sprayMs = svs.time - bot.lastShotAt < 300 ? svs.time - bot.sprayStart : 0;
    if (const WeaponDef *held = HeldWeapon(ps); held && held->fireType != WEAPON_FIRETYPE_FULLAUTO)
        sprayMs /= 2;
    kb::StepAim(bot.aim, bot.view, target, bot.kick.angles, svs.time, seconds, skill, bot.rng, sprayMs);
    if (focus.kind != kb::FocusKind::Seen)
        return 0;

    bot.lastFightTime = svs.time;
    const WeaponDef *def = HeldWeapon(ps);
    if (!IsGun(def))
        return kb::StepStrafe(bot.strafe, svs.time, skill, bot.rng);

    const float eye[3] = {ps->origin[0], ps->origin[1], ps->origin[2] + ps->viewHeightCurrent};
    const float distance = std::sqrt((place[0] - eye[0]) * (place[0] - eye[0]) + (place[1] - eye[1]) * (place[1] - eye[1])
                                     + (place[2] - eye[2]) * (place[2] - eye[2]));

    // The sight for a scoped gun, or a far player; shots wait until it is up.
    const bool scoped = def->overlayReticle != WEAPOVERLAYRETICLE_NONE;
    const bool sight = def->aimDownSight && (scoped || distance > skill.adsRange);
    if (sight)
        cmd.buttons |= BUTTON_ADS;
    // Entering the sight throws the view off by the weapon's ADS view error
    // (CG_PredictPlayerState); the hand has to find the player again.
    if (ps->fWeaponPosFrac >= 1.0f)
    {
        if (!bot.adsErrorDone && def->adsViewErrorMax != 0.0f)
        {
            const float size = bot.rng.Range(def->adsViewErrorMin, def->adsViewErrorMax);
            const float angle = bot.rng.Unit() * 6.28318530718f;
            bot.view.angles[0] = kb::ClampPitch(bot.view.angles[0] + std::sin(angle) * size);
            bot.view.angles[1] = kb::AngleNormalize180(bot.view.angles[1] + std::cos(angle) * size);
        }
        bot.adsErrorDone = true;
    }
    else
    {
        bot.adsErrorDone = false;
    }

    // Close enough to knife, with the hand on them: a player knifes (one
    // hit kills from behind or hurt, two from the front).
    const float meleeReach = (player_meleeRange ? player_meleeRange->current.value : 64.0f) + 8.0f;
    if (distance < meleeReach && svs.time >= bot.aim.startMs)
    {
        const float off = std::fmax(std::fabs(kb::AngleDelta(bot.view.angles[0], target[0])),
                                    std::fabs(kb::AngleDelta(bot.view.angles[1], target[1])));
        if (off < 25.0f && !bot.meleeHeld && svs.time >= bot.nextMelee)
        {
            cmd.buttons |= BUTTON_MELEE;
            bot.meleeHeld = true;
            bot.nextMelee = svs.time + static_cast<int>(bot.rng.Range(450.0f, 700.0f));
            if (LogOn())
                Com_Printf(CON_CHANNEL_SERVER, "bots: %d knifes %d, %.0f units\n", clientNum, focus.entity, distance);
            return kb::StepStrafe(bot.strafe, svs.time, skill, bot.rng);
        }
    }

    const int clip = ps->ammoclip[BG_ClipForWeapon(ps->weapon)];
    const bool ready = clip > 0 && (!sight || ps->fWeaponPosFrac >= 0.9f);
    const bool onTarget =
        ready && kb::OnTarget(bot.aim, bot.view, bot.kick.angles, target, distance, svs.time, skill);
    const bool automatic = def->fireType == WEAPON_FIRETYPE_FULLAUTO;
    if (kb::StepTrigger(bot.trigger, onTarget, automatic, distance, svs.time, skill, bot.rng))
    {
        cmd.buttons |= BUTTON_ATTACK;
        if (svs.time - bot.lastShotAt >= 300)
            bot.sprayStart = svs.time;
        bot.lastShotAt = svs.time;
        if (!bot.firstShotLogged)
        {
            bot.firstShotLogged = true;
            if (bot_log && bot_log->current.integer)
                Com_Printf(CON_CHANNEL_SERVER,
                           "bots: %d fired at %d, %d ms after noticing, %d ms after it came into view, %.0f units\n",
                           clientNum, focus.entity, svs.time - contact.noticedAt,
                           contact.noticedAfterMs + (svs.time - contact.noticedAt), distance);
        }
    }
    return kb::StepStrafe(bot.strafe, svs.time, skill, bot.rng);
}

// ---------------------------------------------------------------------------
// Deciding (bot_mind.h) and the map sense (bot_tactics.cpp).

bool IsReloading(const playerState_s *ps)
{
    return ps->weaponstate >= WEAPON_RELOADING && ps->weaponstate <= WEAPON_RELOAD_END;
}

// The HUD's hit indicator: when the bot was hit, and from which way (the game
// sends the direction of the hit in the player state for the client to draw).
void NoticeHits(BotSlot &bot, const playerState_s *ps)
{
    if (!bot.damageKnown)
    {
        bot.damageEvent = ps->damageEvent;
        bot.damageKnown = true;
        return;
    }
    if (ps->damageEvent == bot.damageEvent)
        return;
    bot.damageEvent = ps->damageEvent;
    bot.hurtAt = svs.time;
    bot.hurtFromKnown = false;
    if (ps->damageYaw == 255 && ps->damagePitch == 255)
        return; // hurt by the world: no direction
    const float yaw = static_cast<float>(ps->damageYaw) * (360.0f / 256.0f) * (3.14159265358979f / 180.0f);
    bot.hurtFrom[0] = ps->origin[0] + std::cos(yaw) * 600.0f;
    bot.hurtFrom[1] = ps->origin[1] + std::sin(yaw) * 600.0f;
    bot.hurtFrom[2] = ps->origin[2] + ps->viewHeightCurrent;
    bot.hurtFromKnown = true;
}

bool RecentlyHurt(const BotSlot &bot, int withinMs)
{
    return svs.time - bot.hurtAt < withinMs;
}

// Where the bot thinks the enemy it worries about is, at eye height: the one
// it sees or remembers, else a sound, else the way it was hit from.
bool Threat(const BotSlot &bot, const kb::Focus &focus, float *out)
{
    if (focus.kind == kb::FocusKind::Seen || focus.kind == kb::FocusKind::Remembered)
    {
        const float *seen = bot.perception.contacts[focus.entity].lastSeen;
        out[0] = seen[0];
        out[1] = seen[1];
        out[2] = seen[2] + 18.0f; // the chest it saw, up to the eyes
        return true;
    }
    if (focus.kind == kb::FocusKind::Heard)
    {
        out[0] = focus.position[0];
        out[1] = focus.position[1];
        out[2] = focus.position[2] + kb::kHeadHeight;
        return true;
    }
    if (bot.hurtFromKnown && RecentlyHurt(bot, 5000))
    {
        out[0] = bot.hurtFrom[0];
        out[1] = bot.hurtFrom[1];
        out[2] = bot.hurtFrom[2];
        return true;
    }
    return false;
}

// Where a hunt goes: where the bot last saw the enemy, or heard one, or the
// way it was hit from.
bool HuntPlace(const BotSlot &bot, const kb::Focus &focus, float *out)
{
    if (focus.kind == kb::FocusKind::Remembered || focus.kind == kb::FocusKind::Heard)
    {
        std::memcpy(out, focus.position, sizeof(focus.position));
        return true;
    }
    if (bot.hurtFromKnown && RecentlyHurt(bot, 5000))
    {
        out[0] = bot.hurtFrom[0];
        out[1] = bot.hurtFrom[1];
        out[2] = bot.hurtFrom[2] - kb::kHeadHeight;
        return true;
    }
    return false;
}

team_t TeamOf(int clientNum);

// Calls fn(clientNum, slot) for each bot on `clientNum`'s team that is
// playing (none in free-for-all).
template <typename Fn>
void ForTeammateBots(int clientNum, Fn fn)
{
    const team_t team = TeamOf(clientNum);
    if (team != TEAM_AXIS && team != TEAM_ALLIES)
        return;
    for (int other = 0; other < kMaxBotSlots && other < sv_maxclients->current.integer; ++other)
    {
        const BotSlot &mate = s_slots[other];
        if (other == clientNum || !mate.active || svs.time - mate.lastTime > 1000 || TeamOf(other) != team)
            continue;
        fn(other, mate);
    }
}

// How crowded `place` is with the bot's team: teammates (people too) within
// 400 units of it, and teammate bots on their way to within 500 of it.
float Crowding(int clientNum, const float *place)
{
    const team_t team = TeamOf(clientNum);
    if (team != TEAM_AXIS && team != TEAM_ALLIES)
        return 0.0f;
    float crowd = 0.0f;
    for (int other = 0; other < sv_maxclients->current.integer; ++other)
    {
        if (other == clientNum || TeamOf(other) != team)
            continue;
        const gentity_s *ent = SV_GentityNum(other);
        if (!ent || !ent->client || ent->client->ps.pm_type != PM_NORMAL)
            continue;
        if (Distance2D(ent->client->ps.origin, place) < 400.0f)
            crowd += 0.5f;
    }
    ForTeammateBots(clientNum, [&](int, const BotSlot &mate) {
        if (mate.hasGoal && Distance2D(mate.goal, place) < 500.0f)
            crowd += 1.0f;
    });
    return crowd;
}

// A place to roam to: the most interesting of a few reachable ones, by the
// bot's own memory of where it has been and met enemies, away from where its
// teammates are and are going (a team spreads out rather than moving as a
// pack).
bool RoamGoal(BotSlot &bot, int clientNum, const kb::NavMesh &nav, const float *origin, float *out)
{
    float bestScore = -1000.0f;
    bool found = false;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float place[3];
        if (!nav.RandomReachablePoint(origin, NextUnit, &bot.rng, place))
            continue;
        if (Distance2D(origin, place) < 300.0f)
            continue;
        found = true;
        const float score = bot.areas.Interest(place, svs.time, bot.personality) + bot.rng.Range(0.0f, 0.3f)
                            - 0.5f * Crowding(clientNum, place);
        if (score > bestScore)
        {
            bestScore = score;
            std::memcpy(out, place, sizeof(place));
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Grenades (bot_grenades.h).

// A frag the bot carries, or 0.
int FragWeapon(const playerState_s *ps)
{
    return BG_GetFirstAvailableOffhand(ps, OFFHAND_CLASS_FRAG_GRENADE);
}

// Whether the arc from `eye` clears the map until it is close to `target`
// (it may bounce off what is right at the target).
bool ArcClear(const kb::ThrowParams &params, const float *eye, float yaw, float pitchUp, float distance,
              const float *target)
{
    float arc[64][3];
    const int count = kb::ArcPoints(params, eye, yaw, pitchUp, distance, 0.05f, arc, 64);
    const float *from = eye;
    for (int i = 0; i < count; ++i)
    {
        const float *to = arc[i];
        const float left = std::hypot(to[0] - target[0], to[1] - target[1]);
        if (left > 96.0f && !Bot_ClearLine(from, to, nullptr))
            return false;
        from = to;
    }
    return true;
}

// A player throws a frag where an enemy just went out of sight. Once per
// enemy lost, a bot thinks about it: the enemy it remembers was seen a
// moment ago, at a throwing distance, out of sight now, and the arc to where
// they were clears the map. Bolder bots throw more often.
void ConsiderGrenade(BotSlot &bot, int clientNum, const playerState_s *ps, const kb::Focus &focus)
{
    if (bot.grenadePhase != 0 || svs.time < bot.nextGrenade || focus.kind != kb::FocusKind::Remembered)
        return;
    const kb::Contact &contact = bot.perception.contacts[focus.entity];
    const int lostMs = svs.time - contact.lastSeenAt;
    if (lostMs < 400 || lostMs > 3500 || bot.grenadeThoughtAt == contact.lastSeenAt)
        return;
    bot.grenadeThoughtAt = contact.lastSeenAt;
    if (IsReloading(ps) || ps->weaponstate != WEAPON_READY)
        return;
    const int frag = FragWeapon(ps);
    if (!frag)
        return;
    const float eye[3] = {ps->origin[0], ps->origin[1], ps->origin[2] + ps->viewHeightCurrent};
    // Their feet: the place it saw is their chest.
    const float target[3] = {contact.lastSeen[0], contact.lastSeen[1], contact.lastSeen[2] - 40.0f};
    const float distance = std::hypot(target[0] - eye[0], target[1] - eye[1]);
    if (distance < 250.0f || distance > 1000.0f)
        return;
    if (bot.rng.Unit() > 0.2f + 0.5f * bot.personality.aggression)
        return;

    const WeaponDef *def = BG_GetWeaponDef(frag);
    kb::ThrowParams params;
    params.speed = static_cast<float>(def->iProjectileSpeed);
    params.speedUp = static_cast<float>(def->iProjectileSpeedUp);
    params.speedForward = static_cast<float>(def->iProjectileSpeedForward);
    params.gravity = ps->gravity > 0 ? static_cast<float>(ps->gravity) : 800.0f;
    float pitchUp = 0.0f;
    if (!kb::ThrowPitch(params, distance, target[2] + 8.0f - eye[2], &pitchUp))
        return;
    const float yaw = std::atan2(target[1] - eye[1], target[0] - eye[0]) * (180.0f / 3.14159265358979f);
    if (!ArcClear(params, eye, yaw, pitchUp, distance, target))
        return;

    bot.grenadePhase = 1;
    bot.grenadeView[0] = -pitchUp;
    bot.grenadeView[1] = yaw;
    bot.grenadeUntil = svs.time + 1500; // give up lining it up after this
    bot.grenadeDistance = distance;
    (void)clientNum;
}

// One frame of a throw under way: line up the view, then hold the button a
// moment (the fuse runs, as when a player cooks it) and let go. Returns the
// view to turn to, or nullptr when no throw is under way.
const float *StepGrenade(BotSlot &bot, int clientNum, const playerState_s *ps, usercmd_s &cmd)
{
    if (bot.grenadePhase == 0)
        return nullptr;
    if (bot.grenadePhase == 1)
    {
        const float off = std::fmax(std::fabs(kb::AngleDelta(bot.view.angles[0], bot.grenadeView[0])),
                                    std::fabs(kb::AngleDelta(bot.view.angles[1], bot.grenadeView[1])));
        if (svs.time >= bot.grenadeUntil || !FragWeapon(ps))
        {
            bot.grenadePhase = 0;
            return nullptr;
        }
        if (off < 2.0f)
        {
            bot.grenadePhase = 2;
            bot.grenadeUntil = svs.time + static_cast<int>(bot.rng.Range(250.0f, 600.0f));
        }
        return bot.grenadeView;
    }
    // Holding: the throw goes on letting go.
    if (svs.time < bot.grenadeUntil)
    {
        cmd.buttons |= BUTTON_FRAG;
        return bot.grenadeView;
    }
    bot.grenadePhase = 0;
    bot.nextGrenade = svs.time + static_cast<int>(bot.rng.Range(10000.0f, 20000.0f));
    if (LogOn())
        Com_Printf(CON_CHANNEL_SERVER, "bots: %d throws a grenade %.0f units\n", clientNum, bot.grenadeDistance);
    return bot.grenadeView;
}

// ---------------------------------------------------------------------------
// The team (bot_team.h).

team_t TeamOf(int clientNum)
{
    const gentity_s *ent = SV_GentityNum(clientNum);
    if (!ent || !ent->client)
        return TEAM_FREE;
    return ent->client->sess.cs.team;
}

// The bot's team, or nullptr in free-for-all (every bot for itself) and when
// it spectates.
TeamState *BoardOf(int clientNum)
{
    const team_t team = TeamOf(clientNum);
    if (team != TEAM_AXIS && team != TEAM_ALLIES)
        return nullptr;
    return &s_teams[team];
}

// The objectives on the compass of a player of the team, as the HUD shows
// them: where, and whether the icon asks to take or to hold the place.
void ReadObjectives(TeamState &team, const playerState_s *ps)
{
    team.takeCount = 0;
    team.holdCount = 0;
    for (int i = 0; i < MAX_OBJECTIVES; ++i)
    {
        const objective_t &obj = ps->objective[i];
        if (obj.state != OBJST_ACTIVE && obj.state != OBJST_CURRENT)
            continue;
        if (obj.icon <= 0 || obj.icon >= CS_SERVER_MATERIALS_COUNT)
            continue;
        char material[64];
        SV_GetConfigstring(CS_SERVER_MATERIALS + obj.icon, material, sizeof(material));
        const kb::ObjectiveKind kind = kb::ObjectiveKindFromIcon(material);
        if (kind == kb::ObjectiveKind::None)
            continue;
        // An icon on an entity (a carried flag or bomb) follows it on the
        // compass.
        const float *place = obj.origin;
        if (obj.entNum != ENTITYNUM_NONE)
        {
            const gentity_s *ent = SV_GentityNum(obj.entNum);
            if (ent && ent->r.inuse)
                place = ent->r.currentOrigin;
        }
        float *out = kind == kb::ObjectiveKind::Take ? team.take[team.takeCount++] : team.hold[team.holdCount++];
        out[0] = place[0];
        out[1] = place[1];
        out[2] = place[2];
    }
}

// The commander, once a second for each team: reads the team's objectives
// and gives each of its bots a part (kb::AssignRoles), and an objective to
// the attackers and defenders, spread over the objectives there are.
void Command()
{
    if (s_commandFrame == svs.time)
        return;
    s_commandFrame = svs.time;
    // Every 10 s, how many shots have been fired: the probe sets the game's
    // logged hits against it.
    static int s_shotsLoggedAt = 0;
    if (svs.time - s_shotsLoggedAt >= 10000 || svs.time < s_shotsLoggedAt)
    {
        s_shotsLoggedAt = svs.time;
        if (LogOn())
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d shots fired so far\n", Bot_ShotsFired());
    }
    for (int teamIndex : {static_cast<int>(TEAM_AXIS), static_cast<int>(TEAM_ALLIES)})
    {
        TeamState &team = s_teams[teamIndex];
        // A new map, or the brain was off: what the team said is old news.
        if (svs.time < team.lastTime || svs.time - team.lastTime > 5000)
        {
            team.board.Reset();
            team.nextCommand = 0;
            team.sideKnown = false;
        }
        team.lastTime = svs.time;
        if (svs.time < team.nextCommand)
            continue;
        team.nextCommand = svs.time + 1000;

        int members[kMaxBotSlots];
        kb::Personality people[kMaxBotSlots];
        int count = 0;
        for (int clientNum = 0; clientNum < kMaxBotSlots && clientNum < sv_maxclients->current.integer; ++clientNum)
        {
            const BotSlot &bot = s_slots[clientNum];
            if (!bot.active || svs.time - bot.lastTime > 1000 || TeamOf(clientNum) != teamIndex)
                continue;
            members[count] = clientNum;
            people[count] = bot.personality;
            ++count;
        }
        if (!count)
            continue;
        const int oldTake = team.takeCount;
        const int oldHold = team.holdCount;
        ReadObjectives(team, SV_GameClientNum(members[0]));
        // Nearest the team first: the first attackers go to the flags on its
        // side of the map. The side is where the team was when objectives
        // first showed (its spawn), so the order holds while bots spread out.
        if (!team.sideKnown && team.takeCount + team.holdCount > 0)
        {
            team.side[0] = 0.0f;
            team.side[1] = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                const playerState_s *member = SV_GameClientNum(members[i]);
                team.side[0] += member->origin[0] / static_cast<float>(count);
                team.side[1] += member->origin[1] / static_cast<float>(count);
            }
            team.sideKnown = true;
        }
        if (team.sideKnown)
        {
            kb::SortNearestFirst(team.take, team.takeCount, team.side);
            kb::SortNearestFirst(team.hold, team.holdCount, team.side);
        }
        if ((team.takeCount != oldTake || team.holdCount != oldHold) && LogOn())
        {
            Com_Printf(CON_CHANNEL_SERVER, "bots: team %d objectives: take %d, hold %d\n", teamIndex, team.takeCount,
                       team.holdCount);
            for (int i = 0; i < team.takeCount; ++i)
                Com_Printf(CON_CHANNEL_SERVER, "bots: team %d objective to take at %.0f %.0f %.0f\n", teamIndex,
                           team.take[i][0], team.take[i][1], team.take[i][2]);
            for (int i = 0; i < team.holdCount; ++i)
                Com_Printf(CON_CHANNEL_SERVER, "bots: team %d objective to hold at %.0f %.0f %.0f\n", teamIndex,
                           team.hold[i][0], team.hold[i][1], team.hold[i][2]);
        }

        kb::Role roles[kMaxBotSlots];
        kb::AssignRoles(people, count, team.takeCount, team.holdCount, roles);
        int attackers = 0;
        int defenders = 0;
        for (int i = 0; i < count; ++i)
        {
            BotSlot &bot = s_slots[members[i]];
            const float *objective = nullptr;
            if (roles[i] == kb::Role::Attack && team.takeCount)
                objective = team.take[attackers++ % team.takeCount];
            else if (roles[i] == kb::Role::Defend && team.holdCount)
                objective = team.hold[defenders++ % team.holdCount];
            bot.objectiveKnown = objective != nullptr;
            if (objective)
                std::memcpy(bot.objective, objective, sizeof(bot.objective));
            if (roles[i] != bot.role)
            {
                bot.role = roles[i];
                bot.roleGoal = false;
                if (LogOn())
                    Com_Printf(CON_CHANNEL_SERVER, "bots: %d plays %s\n", members[i], kb::RoleName(roles[i]));
            }
        }
    }
}

// Calls out to the team the enemies the bot sees, each at most every few
// seconds; the team hears it after the moment it takes to say. What the team
// said goes into the bot's memory of where enemies were.
void TalkToTeam(BotSlot &bot, int clientNum)
{
    TeamState *team = BoardOf(clientNum);
    if (!team)
        return;
    for (int entity = 0; entity < kb::kMaxContacts && entity < kMaxBotSlots; ++entity)
    {
        const kb::Contact &contact = bot.perception.contacts[entity];
        if (!contact.active || !contact.noticed || !contact.visible)
            continue;
        if (bot.calledAt[entity] && svs.time - bot.calledAt[entity] < 3000)
            continue;
        bot.calledAt[entity] = svs.time;
        const int heardAt = svs.time + static_cast<int>(bot.rng.Range(600.0f, 1200.0f));
        team->board.Report(entity, contact.lastSeen, heardAt);
        if (LogOn())
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d calls out %d\n", clientNum, entity);
    }
    const kb::Callout *calls = team->board.Calls();
    for (int i = 0; i < kb::TeamBoard::kMaxCallouts; ++i)
        if (kb::TeamBoard::Heard(calls[i], svs.time, kb::TeamBoard::kLifeMs))
            bot.areas.EnemyAt(calls[i].place, calls[i].heardAtMs);
}

// A place on the mesh at `place`, or near it.
bool PlaceOnMesh(BotSlot &bot, const kb::NavMesh &nav, const float *place, float *out)
{
    if (nav.Nearest(place, 128.0f, out))
        return true;
    return nav.RandomPointNear(place, 200.0f, NextUnit, &bot.rng, out);
}

enum class RolePlan
{
    Default, // the part has nothing to say: roam
    Keep,    // keep going where it goes, or hold where it is
    NewGoal, // bot.goal is set: go there
};

// Starts a hold at a place the part sent the bot to, facing `facing`.
void HoldFor(BotSlot &bot, int untilMs, const float *facing, bool crouched)
{
    bot.holdUntil = untilMs;
    bot.holdCrouched = crouched;
    bot.holdProne = false;
    bot.holdFacingKnown = facing != nullptr;
    if (facing && facing != bot.holdFacing)
        std::memcpy(bot.holdFacing, facing, sizeof(bot.holdFacing));
}

// Where the bot's part sends it when nothing more pressing is on: push to a
// callout, anchor a spot overlooking one, attack or defend its objective.
RolePlan PlanRole(BotSlot &bot, int clientNum, const kb::NavMesh &nav, const float *origin, bool arrived)
{
    TeamState *team = BoardOf(clientNum);
    const bool wasAtObjective = bot.atObjective;
    bot.atObjective = false;
    // Its last goal could not be reached: roam a moment first.
    if (svs.time < bot.roleRetryAt)
        return RolePlan::Default;
    switch (bot.role)
    {
    case kb::Role::Roam:
        return RolePlan::Default;

    case kb::Role::Push:
    {
        const kb::Callout *call = team ? team->board.Freshest(svs.time, 10000) : nullptr;
        if (!call || call->heardAtMs <= bot.pushedCall)
            return RolePlan::Default;
        if (Distance2D(origin, call->place) < 300.0f)
        {
            bot.pushedCall = call->heardAtMs; // got there: nobody
            bot.roleGoal = false;
            return RolePlan::Default;
        }
        if (bot.roleGoal && Distance2D(bot.goal, call->place) < 400.0f && !arrived)
            return RolePlan::Keep;
        // Teammates already on their way there: two at most go, and the second
        // comes from another side of it.
        int others = 0;
        ForTeammateBots(clientNum, [&](int, const BotSlot &mate) {
            if (mate.roleGoal && mate.role == kb::Role::Push && Distance2D(mate.goal, call->place) < 400.0f)
                ++others;
        });
        if (others >= 2)
        {
            bot.pushedCall = call->heardAtMs;
            return RolePlan::Default;
        }
        if (others == 1)
        {
            float place[3];
            if (!nav.RandomPointNear(call->place, 400.0f, NextUnit, &bot.rng, place)
                || Crowding(clientNum, place) >= 1.0f || !PlaceOnMesh(bot, nav, place, bot.goal))
            {
                bot.pushedCall = call->heardAtMs;
                return RolePlan::Default;
            }
        }
        else if (!PlaceOnMesh(bot, nav, call->place, bot.goal))
            return RolePlan::Default;
        bot.roleGoal = true;
        bot.holdUntil = 0;
        if (LogOn())
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d pushes to a callout %.0f units away\n", clientNum,
                       Distance2D(origin, call->place));
        return RolePlan::NewGoal;
    }

    case kb::Role::Anchor:
    {
        if (bot.roleGoal && svs.time < bot.roleUntil)
        {
            if (arrived && svs.time >= bot.holdUntil)
                HoldFor(bot, bot.roleUntil, bot.holdFacingKnown ? bot.holdFacing : nullptr, bot.holdCrouched);
            return RolePlan::Keep;
        }
        bot.roleGoal = false;
        const kb::Callout *call = team ? team->board.Nearest(origin, svs.time) : nullptr;
        if (!call)
            return RolePlan::Default;
        const float eye[3] = {call->place[0], call->place[1], call->place[2] + 18.0f};
        if (!Bot_FindOverwatch(origin, eye, call->place, 1200.0f, nav, bot.rng, bot.goal))
            return RolePlan::Default;
        bot.roleGoal = true;
        bot.roleUntil =
            svs.time + static_cast<int>(bot.rng.Range(10000.0f, 25000.0f) * (0.5f + bot.personality.patience));
        bot.holdUntil = 0;
        bot.holdFacingKnown = true;
        std::memcpy(bot.holdFacing, eye, sizeof(eye));
        bot.holdCrouched = bot.rng.Unit() < 0.4f;
        if (LogOn())
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d anchors %.0f units from a callout\n", clientNum,
                       Distance2D(bot.goal, call->place));
        return RolePlan::NewGoal;
    }

    case kb::Role::Attack:
    {
        if (!bot.objectiveKnown)
            return RolePlan::Default;
        if (bot.roleGoal && Distance2D(bot.goal, bot.objective) < 150.0f)
        {
            if (arrived)
            {
                // There: stay on it (a flag captures while someone stands
                // in it) and press use (a bomb plants or defuses on it), in
                // the stance it chose on the way: many players lie down on a
                // flag and wait for whoever comes to take it back.
                const bool prone = bot.holdProne;
                HoldFor(bot, svs.time + 1000, nullptr, bot.holdCrouched);
                bot.holdProne = prone;
                bot.atObjective = Distance2D(origin, bot.objective) < 96.0f;
                if (bot.atObjective && !wasAtObjective && LogOn())
                    Com_Printf(CON_CHANNEL_SERVER, "bots: %d is on an objective, %s\n", clientNum,
                               bot.holdProne ? "prone" : bot.holdCrouched ? "crouched" : "standing");
                if (!bot.atObjective && !bot.shortLogged && LogOn())
                {
                    bot.shortLogged = true;
                    Com_Printf(CON_CHANNEL_SERVER, "bots: %d stopped %.0f units from an objective at %.0f %.0f %.0f\n",
                               clientNum, Distance2D(origin, bot.objective), origin[0], origin[1], origin[2]);
                }
            }
            return RolePlan::Keep;
        }
        if (!PlaceOnMesh(bot, nav, bot.objective, bot.goal))
            return RolePlan::Default;
        bot.roleGoal = true;
        bot.holdUntil = 0;
        bot.shortLogged = false;
        // How it will wait there: prone (the patient more often), crouched or
        // standing.
        bot.holdProne = bot.rng.Unit() < 0.3f + 0.5f * bot.personality.patience;
        bot.holdCrouched = !bot.holdProne && bot.rng.Unit() < 0.6f;
        if (LogOn())
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d attacks an objective %.0f units away\n", clientNum,
                       Distance2D(origin, bot.objective));
        return RolePlan::NewGoal;
    }

    case kb::Role::Defend:
    {
        if (!bot.objectiveKnown)
            return RolePlan::Default;
        if (bot.roleGoal && svs.time < bot.roleUntil && bot.holdFacingKnown
            && Distance2D(bot.holdFacing, bot.objective) < 150.0f)
        {
            if (arrived && svs.time >= bot.holdUntil)
                HoldFor(bot, bot.roleUntil, bot.holdFacing, bot.holdCrouched);
            return RolePlan::Keep;
        }
        const float watched[3] = {bot.objective[0], bot.objective[1], bot.objective[2] + 40.0f};
        if (!Bot_FindOverwatch(origin, watched, bot.objective, 900.0f, nav, bot.rng, bot.goal)
            && !nav.RandomPointNear(bot.objective, 300.0f, NextUnit, &bot.rng, bot.goal))
            return RolePlan::Default;
        bot.roleGoal = true;
        bot.roleUntil =
            svs.time + static_cast<int>(bot.rng.Range(12000.0f, 25000.0f) * (0.5f + bot.personality.patience));
        bot.holdUntil = 0;
        bot.holdFacingKnown = true;
        std::memcpy(bot.holdFacing, watched, sizeof(watched));
        bot.holdCrouched = bot.rng.Unit() < 0.4f;
        if (LogOn())
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d defends an objective from %.0f units\n", clientNum,
                       Distance2D(bot.goal, bot.objective));
        return RolePlan::NewGoal;
    }
    }
    return RolePlan::Default;
}

const char *CoverReason(const kb::Situation &s)
{
    if (s.reloading || s.clip < 0.25f)
        return "reloading";
    if (s.health < 0.8f)
        return "hurt";
    return "outnumbered";
}

// This frame's decision: what the bot knows goes into a Situation, the Mind
// picks an intent, and the intent picks where to walk. Looks for cover when a
// fight is on.
void Decide(BotSlot &bot, int clientNum, const playerState_s *ps, const kb::Focus &focus, const kb::NavMesh *nav,
            bool fighting)
{
    const float *origin = ps->origin;
    float threat[3];
    const bool threatKnown = Threat(bot, focus, threat);

    // What the bot remembers of the map: where it has been and met enemies,
    // and where its team said they were.
    TalkToTeam(bot, clientNum);
    bot.areas.Visit(origin, svs.time);
    if (focus.kind != kb::FocusKind::None)
        bot.areas.EnemyAt(focus.position, svs.time);
    if (bot.hurtFromKnown && bot.hurtAt == svs.time)
        bot.areas.EnemyAt(bot.hurtFrom, svs.time);

    // Cover from the threat, while there is a fight or a hit to hide from.
    if (!threatKnown || !CombatOn())
    {
        bot.coverKnown = false;
    }
    else if (nav && svs.time >= bot.nextCoverSearch
             && (focus.kind == kb::FocusKind::Seen || RecentlyHurt(bot, 3000) || bot.mind.intent == kb::Intent::Cover))
    {
        bot.nextCoverSearch = svs.time + 1000;
        if (!(bot.coverKnown && Bot_HiddenFrom(bot.cover, threat)))
            bot.coverKnown = Bot_FindCover(origin, threat, *nav, bot.rng, bot.cover);
    }

    kb::Situation s;
    s.nowMs = svs.time;
    const int maxHealth = ps->stats[STAT_MAX_HEALTH] > 0 ? ps->stats[STAT_MAX_HEALTH] : 100;
    s.health = static_cast<float>(ps->stats[STAT_HEALTH]) / static_cast<float>(maxHealth);
    const WeaponDef *def = HeldWeapon(ps);
    if (IsGun(def) && def->iClipSize > 0)
    {
        s.clip = static_cast<float>(ps->ammoclip[BG_ClipForWeapon(ps->weapon)]) / static_cast<float>(def->iClipSize);
        s.canReload = ps->ammo[BG_AmmoForWeapon(ps->weapon)] > 0;
    }
    s.reloading = IsReloading(ps);
    for (const kb::Contact &contact : bot.perception.contacts)
        if (contact.active && contact.noticed && contact.visible)
            ++s.seenEnemies;
    s.enemyInView = fighting && focus.kind == kb::FocusKind::Seen;
    s.enemyRemembered = focus.kind == kb::FocusKind::Remembered;
    if (s.enemyRemembered)
        s.rememberedConfidence = bot.perception.Confidence(focus.entity, svs.time, Senses());
    s.heardSomething = focus.kind == kb::FocusKind::Heard || (bot.hurtFromKnown && RecentlyHurt(bot, 3000));
    // A lead where it found no way to go: a player hears someone on the floor
    // above and does not stand under them; it keeps on with what it did.
    if (float lead[3]; svs.time < bot.noWayUntil && !s.enemyInView && HuntPlace(bot, focus, lead)
                       && Distance2D(lead, bot.noWayTo) < 300.0f && std::fabs(lead[2] - bot.noWayTo[2]) < 96.0f)
    {
        s.heardSomething = false;
        s.enemyRemembered = false;
    }
    s.hurtAgoMs = svs.time - bot.hurtAt;
    s.coverKnown = bot.coverKnown;
    s.inCover = bot.coverKnown && Distance2D(origin, bot.cover) < 40.0f && std::fabs(origin[2] - bot.cover[2]) < 48.0f;
    s.forObjective = bot.role == kb::Role::Attack && bot.objectiveKnown;
    // Teammates who went after the same lead before this bot did.
    if (float lead[3]; !s.enemyInView && HuntPlace(bot, focus, lead))
    {
        const bool hunting = bot.mind.intent == kb::Intent::Hunt;
        ForTeammateBots(clientNum, [&](int other, const BotSlot &mate) {
            if (mate.mind.intent != kb::Intent::Hunt || !mate.hasGoal || Distance2D(mate.goal, lead) > 600.0f)
                return;
            if (hunting && (mate.mind.sinceMs > bot.mind.sinceMs
                            || (mate.mind.sinceMs == bot.mind.sinceMs && other > clientNum)))
                return;
            ++s.teammatesHunting;
        });
    }

    const kb::Intent intent = bot.mind.Decide(s, bot.personality);
    if (bot.mind.changed && bot_log && bot_log->current.integer)
    {
        if (intent == kb::Intent::Cover)
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d takes cover %.0f units away (%s)\n", clientNum,
                       Distance2D(origin, bot.cover), CoverReason(s));
        else if (intent == kb::Intent::Hunt)
            Com_Printf(CON_CHANNEL_SERVER, "bots: %d hunts (%s)\n", clientNum,
                       s.enemyRemembered ? "seen" : focus.kind == kb::FocusKind::Heard ? "heard" : "hit");
    }
    if (!nav)
        return;

    // Where the intent goes.
    const bool arrived = bot.hasGoal && bot.pathCount > 0 && bot.pathIndex >= bot.pathCount;
    bool replan = bot.mind.changed || !bot.hasGoal || bot.goalFor != intent;
    switch (intent)
    {
    case kb::Intent::Fight:
        // Keep walking the way it was on (bot_combat.h may strafe instead).
        return;
    case kb::Intent::Cover:
        bot.roleGoal = false;
        if (!bot.coverKnown)
            return;
        if (Distance2D(bot.goal, bot.cover) > 32.0f)
            replan = true;
        std::memcpy(bot.goal, bot.cover, sizeof(bot.goal));
        if (arrived && !replan)
            return; // in cover: wait there
        break;
    case kb::Intent::Hunt:
    {
        bot.roleGoal = false;
        float place[3];
        if (!HuntPlace(bot, focus, place))
            return;
        if (arrived && !replan && Distance2D(origin, bot.goal) < 128.0f)
        {
            // Got there and nobody is: the lead is cold.
            if (focus.kind == kb::FocusKind::Remembered || focus.kind == kb::FocusKind::Heard)
                bot.perception.Forget(focus.entity);
            bot.hurtFromKnown = false;
            return;
        }
        if (Distance2D(bot.goal, place) > 128.0f)
            replan = true;
        if (replan)
            std::memcpy(bot.goal, place, sizeof(place));
        break;
    }
    case kb::Intent::Roam:
    {
        // The bot's part in the team first.
        const RolePlan plan = PlanRole(bot, clientNum, *nav, origin, arrived);
        if (plan == RolePlan::NewGoal)
        {
            replan = true;
            break;
        }
        if (plan == RolePlan::Keep)
            break;
        bot.holdFacingKnown = false;
        if (arrived || replan)
        {
            if (!RoamGoal(bot, clientNum, *nav, origin, bot.goal))
            {
                bot.hasGoal = false;
                return;
            }
            // Now and then a bot that got somewhere stays a while and holds
            // the way it will go next, crouched or standing.
            const float hold = 0.15f + 0.45f * bot.personality.patience * (1.0f - bot.personality.aggression);
            if (arrived && bot.rng.Unit() < hold)
            {
                bot.holdUntil =
                    svs.time + static_cast<int>(bot.rng.Range(2500.0f, 7000.0f) * (0.5f + bot.personality.patience));
                bot.holdCrouched = bot.rng.Unit() < 0.5f;
                bot.holdProne = false;
            }
            replan = true;
        }
        break;
    }
    }

    if (replan || (bot.pathIndex >= bot.pathCount && !arrived && svs.time >= bot.nextPlanTime))
    {
        bot.hasGoal = true;
        bot.goalFor = intent;
        const bool walkable = PlanWalk(bot, *nav, origin, bot.goal);
        // A hunt whose way ends short of the lead, above or below it (on
        // another floor the way does not reach): no way there. Standing under
        // a sound and staring at the ceiling is what a player does not do.
        if (walkable && intent == kb::Intent::Hunt)
        {
            const float *end = bot.path[bot.pathCount - 1];
            const float floorAt = bot.goal[2] - kb::kHeadHeight;
            if (std::fabs(end[2] - floorAt) > 80.0f && Distance2D(end, bot.goal) < 400.0f)
            {
                std::memcpy(bot.noWayTo, bot.goal, sizeof(bot.noWayTo));
                bot.noWayUntil = svs.time + 10000;
                bot.pathCount = 0;
                bot.pathIndex = 0;
                bot.hasGoal = false;
                if (LogOn())
                    Com_Printf(CON_CHANNEL_SERVER, "bots: %d finds no way to a lead %.0f units %s\n", clientNum,
                               std::fabs(end[2] - floorAt), end[2] < floorAt ? "above" : "below");
                return;
            }
        }
        if (!walkable)
        {
            bot.nextPlanTime = svs.time + 1000;
            if (intent == kb::Intent::Roam)
                bot.hasGoal = false;
            if (bot.roleGoal)
            {
                bot.roleGoal = false;
                bot.roleRetryAt = svs.time + 3000;
                if (bot.role == kb::Role::Attack && LogOn())
                    Com_Printf(CON_CHANNEL_SERVER, "bots: %d finds no way to an objective at %.0f %.0f %.0f\n",
                               clientNum, bot.goal[0], bot.goal[1], bot.goal[2]);
            }
        }
    }
}

// The corner of the bot's way it pre-aims, refreshed four times a second.
void UpdatePreAim(BotSlot &bot, const playerState_s *ps)
{
    if (svs.time < bot.nextPreAim)
        return;
    bot.nextPreAim = svs.time + 250;
    const float eye[3] = {ps->origin[0], ps->origin[1], ps->origin[2] + ps->viewHeightCurrent};
    bot.preAimKnown = kb::PreAimPoint(eye, ps->origin, bot.path, bot.pathCount, bot.pathIndex, Bot_ClearLine,
                                      nullptr, bot.preAim);
}

} // namespace

void Bot_RegisterDvars()
{
    bot_brain = Dvar_RegisterInt(
        "bot_brain",
        0,
        DvarLimits(0, 1),
        DVAR_NOFLAG,
        "Which brain test clients use: 0 the retail random inputs, 1 KisakCOD's bots (docs/bots.md)");
    bot_log = Dvar_RegisterInt(
        "bot_log",
        0,
        DvarLimits(0, 1),
        DVAR_NOFLAG,
        "1 prints to the console each player a KisakCOD bot notices, and after how long in view (docs/bots.md)");
    bot_combat = Dvar_RegisterInt(
        "bot_combat",
        1,
        DvarLimits(0, 1),
        DVAR_NOFLAG,
        "0 KisakCOD's bots never fire, 1 they fight the players they see (docs/bots.md)");
}

int Bot_Ping(int clientNum)
{
    if (!bot_brain || bot_brain->current.integer != 1 || clientNum < 0 || clientNum >= kMaxBotSlots)
        return -1;
    const BotSlot &bot = s_slots[clientNum];
    if (!bot.active || svs.time - bot.lastTime > 1000)
        return -1;
    // The brain runs in the server: no network between them.
    return 0;
}

bool Bot_UserMove(client_t *cl)
{
    if (!bot_brain || bot_brain->current.integer != 1)
        return false;
    if (!cl->gentity)
        return true;

    const int clientNum = cl - svs.clients;
    if (clientNum < 0 || clientNum >= kMaxBotSlots)
        return false;

    BotSlot &bot = s_slots[clientNum];
    const uint32_t port = cl->header.netchan.remoteAddress.port;
    // A new bot in the slot, or one the brain has not driven for a while
    // (bot_brain was off, or a map change): start afresh.
    if (!bot.active || bot.port != port || svs.time < bot.lastTime || svs.time - bot.lastTime > 1000)
    {
        bot = BotSlot();
        bot.active = true;
        bot.port = port;
        bot.lastTime = svs.time;
        bot.rng.Seed(0x6b6973u ^ (port * 2654435761u) ^ static_cast<uint32_t>(clientNum));
        bot.nextSense = svs.time + (clientNum * 37) % kSenseMs;
        bot.personality = kb::RandomPersonality(bot.rng);
        bot.areas.Reset();
        bot.mind.Reset(svs.time);
    }

    const playerState_s *ps = SV_GameClientNum(clientNum);
    // Moved further than a player runs in a frame: a spawn or a teleport, so
    // the old way no longer starts here.
    if (Distance2D(ps->origin, bot.lastOrigin) > 128.0f || std::fabs(ps->origin[2] - bot.lastOrigin[2]) > 128.0f)
    {
        bot.pathCount = 0;
        bot.pathIndex = 0;
    }
    // Faster than a player runs: say how (the probe once saw bots cover 330
    // units in the first second after a round's start).
    {
        const float frameMove = Distance2D(ps->origin, bot.lastOrigin);
        const float speed = std::hypot(ps->velocity[0], ps->velocity[1]);
        if ((speed > 250.0f || (frameMove > 40.0f && frameMove <= 128.0f)) && svs.time >= bot.nextFastLog && LogOn())
        {
            bot.nextFastLog = svs.time + 2000;
            Com_Printf(CON_CHANNEL_SERVER,
                       "bots: %d moves fast: %.0f units in %d ms, velocity %.0f, pm_type %d, pm_flags 0x%x, "
                       "command time %d behind\n",
                       clientNum, frameMove, svs.time - bot.lastTime, speed, ps->pm_type, ps->pm_flags,
                       svs.time - ps->commandTime);
        }
    }
    bot.lastOrigin[0] = ps->origin[0];
    bot.lastOrigin[1] = ps->origin[1];
    bot.lastOrigin[2] = ps->origin[2];
    float seconds = static_cast<float>(svs.time - bot.lastTime) * 0.001f;
    if (seconds < 0.0f)
        seconds = 0.0f;
    bot.lastTime = svs.time;

    usercmd_s cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.serverTime = svs.time;
    cmd.weapon = static_cast<uint8_t>(ps->weapon);

    // The server moved the view since the last command (a pitch limit, a
    // spawn, a killcam, a script's setPlayerAngles): carry on from it. A large
    // move is a new place, so pick something new to look at too.
    const float moved = std::fmax(std::fabs(kb::AngleDelta(ps->viewangles[0], bot.sentView[0])),
                                  std::fabs(kb::AngleDelta(ps->viewangles[1], bot.sentView[1])));
    if (!bot.view.primed || moved > 1.0f)
    {
        const bool newPlace = !bot.view.primed || moved > 10.0f;
        // The view the server holds has the kick in it; the hand's has not.
        kb::PrimeView(bot.view, ps->viewangles[0] - bot.kick.angles[0], ps->viewangles[1] - bot.kick.angles[1]);
        if (newPlace)
        {
            bot.perception.Reset();
            bot.pathCount = 0;
            bot.pathIndex = 0;
            bot.hasGoal = false;
            bot.holdUntil = 0;
            bot.coverKnown = false;
            bot.hurtFromKnown = false;
            bot.roleGoal = false;
            bot.holdFacingKnown = false;
            bot.atObjective = false;
            bot.onObjectiveSince = 0;
            bot.mind.Reset(svs.time);
            bot.look.pitch = bot.view.angles[0];
            bot.look.yaw = bot.view.angles[1];
            bot.look.holdSeconds = 0.0f;
            bot.lookHeld = 0.0f;
        }
    }

    const bool alive = !IsDead(ps) && ps->pm_type == PM_NORMAL;
    if (alive)
    {
        KickView(bot, ps, static_cast<int>(std::lround(seconds * 1000.0f)));
    }
    else
    {
        bot.kick = kb::ViewKick();
        bot.eventKnown = false;
    }

    if (!G_GetClientArchiveTime(clientNum))
    {
        const kb::NavMesh *nav = Bot_Nav();
        if (!alive)
            bot.perception.Reset();
        else
            Sense(bot, clientNum);
        kb::Focus focus;
        if (!IsDead(ps) && ps->pm_type == PM_NORMAL)
        {
            const float eye[3] = {ps->origin[0], ps->origin[1], ps->origin[2] + ps->viewHeightCurrent};
            focus = bot.perception.Choose(eye, svs.time, Senses());
        }
        float watch[2] = {0.0f, 0.0f};
        if (focus.kind != kb::FocusKind::None)
        {
            ViewToward(ps, focus.position, watch);
            // A sound or a memory straight above or below: a player looks
            // toward it, not at the ceiling or the floor.
            if (focus.kind != kb::FocusKind::Seen)
                watch[0] = std::fmax(-35.0f, std::fmin(35.0f, watch[0]));
        }
        const float *watching = focus.kind != kb::FocusKind::None ? watch : nullptr;

        // A fight: the player it sees, or the one it was fighting and has just
        // lost sight of (it keeps its aim where they went).
        const bool fighting = alive && CombatOn()
                              && (focus.kind == kb::FocusKind::Seen
                                  || (focus.kind == kb::FocusKind::Remembered && focus.entity == bot.aim.target));
        if (!fighting && bot.aim.target >= 0)
        {
            bot.aim = kb::AimState();
            bot.aimEndedAt = svs.time;
            bot.trigger = kb::TriggerState();
        }
        if (alive)
            LookAfterWeapon(bot, ps, cmd);

        if (IsDead(ps))
        {
            bot.pathCount = 0;
            bot.pathIndex = 0;
            bot.hasGoal = false;
            bot.holdUntil = 0;
            bot.roleGoal = false;
            bot.holdFacingKnown = false;
            bot.atObjective = false;
            if (bot.onObjectiveSince && LogOn())
                Com_Printf(CON_CHANNEL_SERVER, "bots: %d left an objective after %.1f s (died)\n", clientNum,
                           (svs.time - bot.onObjectiveSince) / 1000.0f);
            bot.onObjectiveSince = 0;
            bot.mind.Reset(svs.time);
            // Tap use now and then: the stock gametypes respawn on it where
            // they wait for a button, and it skips nothing that matters.
            bot.respawnTap = (bot.respawnTap + 1) % 10;
            if (bot.respawnTap == 0)
                cmd.buttons |= BUTTON_USE_RELOAD;
        }
        else if (alive)
        {
            Command();
            if (nav)
                Bot_StepSpots(*nav, ps->origin);
            NoticeHits(bot, ps);
            Decide(bot, clientNum, ps, focus, nav, fighting);
            // How long an attacker stays on the objective it takes, and what
            // it was doing when it left.
            {
                const bool on = bot.role == kb::Role::Attack && bot.objectiveKnown &&
                                Distance2D(ps->origin, bot.objective) < 96.0f;
                if (on && !bot.onObjectiveSince)
                    bot.onObjectiveSince = svs.time ? svs.time : 1;
                else if (!on && bot.onObjectiveSince)
                {
                    if (LogOn())
                        Com_Printf(CON_CHANNEL_SERVER, "bots: %d left an objective after %.1f s (%s, role %s)\n",
                                   clientNum, (svs.time - bot.onObjectiveSince) / 1000.0f,
                                   kb::IntentName(bot.mind.intent), kb::RoleName(bot.role));
                    bot.onObjectiveSince = 0;
                }
            }

            int side = 0;
            // An enemy in view: a throw still being lined up is dropped.
            if (focus.kind == kb::FocusKind::Seen && bot.grenadePhase == 1)
                bot.grenadePhase = 0;
            // The enemy it fought has gone out of sight: a moment to think of
            // a grenade (the hand keeps its aim there meanwhile).
            if (CombatOn())
                ConsiderGrenade(bot, clientNum, ps, focus);
            if (fighting && bot.grenadePhase == 0)
                side = Fight(bot, clientNum, ps, focus, seconds, cmd);
            if (!(cmd.buttons & BUTTON_MELEE))
                bot.meleeHeld = false;
            const float *grenadeView = CombatOn() ? StepGrenade(bot, clientNum, ps, cmd) : nullptr;

            // Not fighting: look at the player it lost or the sound it heard,
            // else pre-aim the corner of its way, else along the way. A throw
            // looks where it throws.
            const float *look = grenadeView ? grenadeView : watching;
            // The fight's hand has the view, unless a throw is being lined up.
            const bool handAims = fighting && !grenadeView;
            float preAimView[2];
            if (!fighting && !look && bot.pathIndex < bot.pathCount)
            {
                UpdatePreAim(bot, ps);
                if (bot.preAimKnown)
                {
                    ViewToward(ps, bot.preAim, preAimView);
                    look = preAimView;
                }
            }

            const bool holding = bot.mind.intent == kb::Intent::Roam && svs.time < bot.holdUntil;
            // Holding for its part in the team: watch what the part is about.
            float facingView[2];
            if (!fighting && !look && holding && bot.holdFacingKnown)
            {
                ViewToward(ps, bot.holdFacing, facingView);
                look = facingView;
            }
            // Prone on a flag it takes: it stays down and fights from there
            // rather than strafing off the flag.
            const bool downOnFlag = bot.holdProne && bot.atObjective && bot.role == kb::Role::Attack &&
                                    (fighting || holding) && Distance2D(ps->origin, bot.objective) < 96.0f;
            if (downOnFlag)
                side = 0;
            bool walked = false;
            if (downOnFlag)
            {
                bot.progressTime = svs.time;
                cmd.buttons |= BUTTON_PRONE;
                if (holding)
                    cmd.buttons |= BUTTON_USE;
            }
            else if (side != 0 && bot.mind.intent != kb::Intent::Cover)
            {
                cmd.rightmove = static_cast<char>(side * 127);
                // A strafe is not being stuck on the way.
                if (bot.pathIndex < bot.pathCount)
                    bot.bestDistance = Distance2D(ps->origin, bot.path[bot.pathIndex]);
                bot.progressTime = svs.time;
            }
            else if (!holding)
            {
                walked = Walk(bot, ps, seconds, cmd, look, handAims);
            }
            else
            {
                bot.progressTime = svs.time;
                if (bot.holdProne)
                    cmd.buttons |= BUTTON_PRONE;
                else if (bot.holdCrouched)
                    cmd.buttons |= BUTTON_CROUCH;
                // On an objective: use it (a bomb plants or defuses while
                // use is held there).
                if (bot.atObjective)
                    cmd.buttons |= BUTTON_USE;
            }

            if (handAims || walked)
            {
                bot.lookHeld = 0.0f;
                bot.look.holdSeconds = 0.0f;
            }
            else if (look)
            {
                kb::StepView(bot.view, look[0], look[1], seconds, WatchMotor());
                bot.lookHeld = 0.0f;
                bot.look.holdSeconds = 0.0f;
            }
            else
            {
                bot.lookHeld += seconds;
                if (bot.lookHeld >= bot.look.holdSeconds)
                {
                    bot.look = kb::NextIdleLook(bot.rng, bot.view.angles[1]);
                    bot.lookHeld = 0.0f;
                }
                kb::StepView(bot.view, bot.look.pitch, bot.look.yaw, seconds, IdleMotor());
            }
        }
    }

    // The command's view is the hand's plus the weapon's kick, as a player's
    // client sends it (CL_CreateCmd adds cgameKickAngles).
    for (int axis = 0; axis < 2; ++axis)
    {
        cmd.angles[axis] = kb::CmdAngleFor(bot.view.angles[axis] + bot.kick.angles[axis], ps->delta_angles[axis]);
        bot.sentView[axis] = kb::ViewForCmdAngle(cmd.angles[axis], ps->delta_angles[axis]);
    }
    cmd.angles[2] = kb::CmdAngleFor(bot.kick.angles[2], ps->delta_angles[2]);

    cl->header.deltaMessage = cl->header.netchan.outgoingSequence - 1;
    SV_ClientThink(cl, &cmd);
    return true;
}
