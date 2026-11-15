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
//   decide    where to go and what to look at (for now: walk to random
//             places on the map's navigation mesh, look around when there is
//             none, watch the player it noticed, remembers or heard, fight
//             the one it sees (bot_combat.h, with bot_combat 1), ask to
//             respawn when dead; later phases add objectives, docs/bots.md);
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
#include "bot_motor.h"
#include "bot_navmesh.h"
#include "bot_perception.h"

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
    kb::ViewKick kick;
    kb::StrafeState strafe;
    int eventSequence = 0;
    bool eventKnown = false;
    bool adsErrorDone = false;
    bool firstShotLogged = false;
    int lastFightTime = -100000;
    bool reloadHeld = false;
    int nextReloadTime = 0;
};

BotSlot s_slots[kMaxBotSlots];

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

// Picks a reachable place and the way there. False when there is none.
bool PlanWalk(BotSlot &bot, const kb::NavMesh &nav, const float *origin)
{
    bot.pathCount = 0;
    bot.pathIndex = 0;
    float goal[3];
    if (!nav.RandomReachablePoint(origin, NextUnit, &bot.rng, goal))
        return false;
    const int count = nav.FindPath(origin, goal, bot.path, BotSlot::kMaxCorners);
    if (count < 2)
        return false;
    bot.pathCount = count;
    bot.pathIndex = 1; // corner 0 is where the bot stands
    bot.bestDistance = Distance2D(origin, bot.path[1]);
    bot.progressTime = svs.time;
    return true;
}

// One server frame of walking: turn toward the next corner, move toward it,
// jump when stuck, find another way when still stuck. Returns false when the
// bot has nowhere to go this frame (it then looks around). With `aiming` the
// view is the fight's and is left alone.
bool Walk(BotSlot &bot, const kb::NavMesh &nav, const playerState_s *ps, float seconds, usercmd_s &cmd,
          const float *watch, bool aiming = false)
{
    const float *origin = ps->origin;
    if (bot.pathIndex >= bot.pathCount)
    {
        if (svs.time < bot.nextPlanTime)
            return false;
        if (!PlanWalk(bot, nav, origin))
        {
            bot.nextPlanTime = svs.time + 1000;
            return false;
        }
    }

    while (bot.pathIndex < bot.pathCount
           && Distance2D(origin, bot.path[bot.pathIndex]) < kCornerReach
           && std::fabs(origin[2] - bot.path[bot.pathIndex][2]) < 48.0f)
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
        // Arrived: look around for a moment, as a player would, then go on.
        bot.nextPlanTime = svs.time + static_cast<int>(bot.rng.Range(500.0f, 2500.0f));
        bot.stuckCount = 0;
        return false;
    }

    const float *corner = bot.path[bot.pathIndex];
    const float distance = Distance2D(origin, corner);
    if (distance < bot.bestDistance - 8.0f)
    {
        bot.bestDistance = distance;
        bot.progressTime = svs.time;
    }
    const int stalled = svs.time - bot.progressTime;
    if (stalled > kReplanAfterMs)
    {
        ++bot.stuckCount;
        bot.pathCount = 0;
        bot.pathIndex = 0;
        bot.nextPlanTime = svs.time;
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
    // on the press only.
    if (stalled > kJumpAfterMs && !bot.jumpHeld)
    {
        cmd.buttons |= BUTTON_JUMP;
        bot.jumpHeld = true;
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
        kb::Engage(bot.aim, focus.entity, contact.noticedAt, svs.time, skill, bot.rng);
        bot.trigger = kb::TriggerState();
        bot.firstShotLogged = false;
    }

    float place[3];
    kb::LeadPlace(contact.lastSeen, contact.velocity, svs.time - contact.lastSeenAt, skill, place);
    float target[2];
    ViewToward(ps, place, target);
    kb::StepAim(bot.aim, bot.view, target, bot.kick.angles, svs.time, seconds, skill, bot.rng);
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

    const int clip = ps->ammoclip[BG_ClipForWeapon(ps->weapon)];
    const bool ready = clip > 0 && (!sight || ps->fWeaponPosFrac >= 0.9f);
    const bool onTarget =
        ready && kb::OnTarget(bot.aim, bot.view, bot.kick.angles, target, distance, svs.time, skill);
    const bool automatic = def->fireType == WEAPON_FIRETYPE_FULLAUTO;
    if (kb::StepTrigger(bot.trigger, onTarget, automatic, distance, svs.time, skill, bot.rng))
    {
        cmd.buttons |= BUTTON_ATTACK;
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
    }

    const playerState_s *ps = SV_GameClientNum(clientNum);
    // Moved further than a player runs in a frame: a spawn or a teleport, so
    // the old way no longer starts here.
    if (Distance2D(ps->origin, bot.lastOrigin) > 128.0f || std::fabs(ps->origin[2] - bot.lastOrigin[2]) > 128.0f)
    {
        bot.pathCount = 0;
        bot.pathIndex = 0;
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
            ViewToward(ps, focus.position, watch);
        const float *watching = focus.kind != kb::FocusKind::None ? watch : nullptr;

        // A fight: the player it sees, or the one it was fighting and has just
        // lost sight of (it keeps its aim where they went).
        const bool fighting = alive && CombatOn()
                              && (focus.kind == kb::FocusKind::Seen
                                  || (focus.kind == kb::FocusKind::Remembered && focus.entity == bot.aim.target));
        if (!fighting && bot.aim.target >= 0)
        {
            bot.aim = kb::AimState();
            bot.trigger = kb::TriggerState();
        }
        if (alive)
            LookAfterWeapon(bot, ps, cmd);

        if (IsDead(ps))
        {
            bot.pathCount = 0;
            bot.pathIndex = 0;
            // Tap use now and then: the stock gametypes respawn on it where
            // they wait for a button, and it skips nothing that matters.
            bot.respawnTap = (bot.respawnTap + 1) % 10;
            if (bot.respawnTap == 0)
                cmd.buttons |= BUTTON_USE_RELOAD;
        }
        else if (fighting)
        {
            const int side = Fight(bot, clientNum, ps, focus, seconds, cmd);
            if (side != 0)
            {
                cmd.rightmove = static_cast<char>(side * 127);
                // A strafe is not being stuck on the way.
                if (bot.pathIndex < bot.pathCount)
                    bot.bestDistance = Distance2D(ps->origin, bot.path[bot.pathIndex]);
                bot.progressTime = svs.time;
            }
            else if (nav)
            {
                Walk(bot, *nav, ps, seconds, cmd, nullptr, true);
            }
            bot.lookHeld = 0.0f;
            bot.look.holdSeconds = 0.0f;
        }
        else if (nav && ps->pm_type == PM_NORMAL && Walk(bot, *nav, ps, seconds, cmd, watching))
        {
            bot.lookHeld = 0.0f;
            bot.look.holdSeconds = 0.0f;
        }
        else if (watching)
        {
            kb::StepView(bot.view, watch[0], watch[1], seconds, WatchMotor());
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
