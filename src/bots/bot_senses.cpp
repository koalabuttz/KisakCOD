// A bot's eyes and ears on the server (docs/bots.md): what a player in the
// bot's place would see and hear on one sense tick, handed to the bot's
// Perception (bot_perception.h), which decides what the bot knows.
//
//   eyes      every other player of the other side that is playing: on the
//             bot's screen (its own view angles), in its potentially visible
//             set and inside the map's fog (SV_inSnapshot, the test the
//             server uses to send a player to a client), and then a clear
//             line from the bot's eye to their head, chest and hips through
//             what blocks a player's sight (MASK_AIMTARGET_VISIBILITY:
//             walls, foliage, no-sight brushes, vehicles).
//   ears      footsteps of a walking, running or sprinting player who is not
//             crouched, prone or wearing Dead Silence (specialty_quieter),
//             and shots, from the fire events in each player's state, quieter
//             with a silenced weapon.
//   compass   a player the HUD shows as a red dot (eFlags 0x400000, set while
//             a script pings them), and every opponent without UAV Jammer
//             (specialty_gpsjammer) at each radar sweep while the bot's own
//             player state says its side has a UAV (radarEnabled).
//   smoke     each smoke grenade's burst, from the event entity
//             G_ExplodeMissile spawns for it, becomes a cloud that hides
//             what is behind it (bot_perception.h).
//
// The bot reads its own player state as its client would. It never reads
// another player's place except through these tests.

#include <universal/q_shared.h>
#include <server_mp/server_mp.h>
#include <server/sv_game.h>
#include <server/sv_world.h>
#include <game_mp/g_main_mp.h>
#include <bgame/bg_local.h>
#include <bgame/bg_public.h>

#include "bot_brain.h"
#include "bot_motor.h"
#include "bot_perception.h"

#include <qcommon/ent.h>

#include <cmath>

namespace
{

namespace kb = kisak::bots;

constexpr int kMaxPlayers = 64;
// A player's compass dot (PlayerCmd_PingPlayer, g_active_mp.cpp).
constexpr int kCompassPingFlag = 0x400000;

// Shots heard this frame and lately, from every player's fire events, so
// each bot hears those since its last sense tick.
struct Shot
{
    int entity = -1;
    int time = 0;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    bool silenced = false;
};
constexpr int kShotRing = 128;
Shot s_shots[kShotRing];
int s_shotCount = 0;
int s_scannedTime = -1;
int s_eventSequence[kMaxPlayers];
bool s_eventSequenceKnown[kMaxPlayers];

// Smoke clouds seen to burst lately, and the burst event entities already
// counted (an event entity lives a few frames).
constexpr int kSmokeRing = 16;
kb::Smoke s_smokes[kSmokeRing];
int s_smokeCount = 0;
int s_smokeEventSeen[MAX_GENTITIES];

// When each bot's compass last swept with a UAV up.
int s_radarSweptAt[kMaxPlayers];

const gclient_s *PlayingClient(int entity)
{
    const gentity_s *ent = SV_GentityNum(entity);
    if (!ent || !ent->client || ent->health <= 0)
        return nullptr;
    const gclient_s *client = ent->client;
    if (client->sess.sessionState != SESS_STATE_PLAYING || client->sess.connected != CON_CONNECTED)
        return nullptr;
    if (client->sess.cs.team == TEAM_SPECTATOR)
        return nullptr;
    return client;
}

bool Opponents(team_t a, team_t b)
{
    return a == TEAM_FREE || b == TEAM_FREE || a != b;
}

kb::Stance StanceOf(const playerState_s &ps)
{
    if (ps.pm_flags & PMF_PRONE)
        return kb::Stance::Prone;
    if (ps.pm_flags & PMF_DUCKED)
        return kb::Stance::Crouched;
    return kb::Stance::Standing;
}

float GroundSpeed(const playerState_s &ps)
{
    return std::hypot(ps.velocity[0], ps.velocity[1]);
}

void Scan(int now)
{
    if (s_scannedTime == now)
        return;
    s_scannedTime = now;
    for (int entity = 0; entity < kMaxPlayers && entity < sv_maxclients->current.integer; ++entity)
    {
        const gclient_s *client = PlayingClient(entity);
        if (!client)
        {
            s_eventSequenceKnown[entity] = false;
            continue;
        }
        const playerState_s &ps = client->ps;
        if (!s_eventSequenceKnown[entity])
        {
            s_eventSequenceKnown[entity] = true;
            s_eventSequence[entity] = ps.eventSequence;
            continue;
        }
        int from = s_eventSequence[entity];
        // Only the last four events are kept (MAX_PS_EVENTS).
        if (ps.eventSequence - from > 4)
            from = ps.eventSequence - 4;
        for (int sequence = from; sequence < ps.eventSequence; ++sequence)
        {
            const int event = ps.events[sequence & 3] & 0xff;
            if (event != EV_FIRE_WEAPON && event != EV_FIRE_WEAPON_LASTSHOT)
                continue;
            Shot &shot = s_shots[s_shotCount % kShotRing];
            ++s_shotCount;
            shot.entity = entity;
            shot.time = now;
            shot.origin[0] = ps.origin[0];
            shot.origin[1] = ps.origin[1];
            shot.origin[2] = ps.origin[2] + ps.viewHeightCurrent;
            const WeaponDef *weapon = ps.weapon ? BG_GetWeaponDef(ps.weapon) : nullptr;
            shot.silenced = weapon && weapon->silenced;
        }
        s_eventSequence[entity] = ps.eventSequence;
    }

    // A new map takes the old one's clouds with it.
    static uint32_t smokeMap = 0;
    if (cm.checksum != smokeMap)
    {
        smokeMap = cm.checksum;
        s_smokeCount = 0;
    }
    // Smoke bursts: G_ExplodeMissile spawns an ET_GENERAL event entity with
    // the grenade's weapon where it bursts.
    for (int entity = kMaxPlayers; entity < sv.num_entities && entity < MAX_GENTITIES; ++entity)
    {
        const gentity_s *ent = SV_GentityNum(entity);
        if (!ent || !ent->r.inuse || ent->s.eType != ET_GENERAL || !ent->s.weapon)
        {
            s_smokeEventSeen[entity] = 0;
            continue;
        }
        if (s_smokeEventSeen[entity])
            continue;
        const WeaponDef *weapon = BG_GetWeaponDef(ent->s.weapon);
        if (!weapon || weapon->offhandClass != OFFHAND_CLASS_SMOKE_GRENADE)
            continue;
        s_smokeEventSeen[entity] = 1;
        kb::Smoke &smoke = s_smokes[s_smokeCount % kSmokeRing];
        ++s_smokeCount;
        smoke.burst[0] = ent->r.currentOrigin[0];
        smoke.burst[1] = ent->r.currentOrigin[1];
        smoke.burst[2] = ent->r.currentOrigin[2];
        smoke.startMs = now;
    }
}

int SmokeCount()
{
    return s_smokeCount < kSmokeRing ? s_smokeCount : kSmokeRing;
}

float Distance(const float *a, const float *b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void HearAt(kb::Perception &perception, int entity, const float *eye, const float *source, float error, int now,
            kb::Rng &rng)
{
    float place[3];
    const float a = rng.Unit();
    const float b = rng.Unit();
    kb::HeardPlace(eye, source, error, a, b, place);
    perception.Hear(entity, place, now);
}

uint32_t PerkBit(const char *name)
{
    const uint32_t index = BG_GetPerkIndexForName(name);
    return index < 32 ? (1u << index) : 0;
}

uint32_t QuietPerkBit()
{
    static const uint32_t bit = PerkBit("specialty_quieter");
    return bit;
}

uint32_t JammerPerkBit()
{
    static const uint32_t bit = PerkBit("specialty_gpsjammer");
    return bit;
}

} // namespace

int Bot_Sense(int clientNum, kb::Perception &perception, const kb::PerceptionParams &params, int now, int lastSense,
              kb::Rng &rng, BotNotice *notices, int maxNotices)
{
    Scan(now);
    const gclient_s *self = PlayingClient(clientNum);
    if (!self)
    {
        perception.Reset();
        return 0;
    }
    const playerState_s &me = self->ps;
    const team_t myTeam = self->sess.cs.team;
    const float seconds = static_cast<float>(now - lastSense) * 0.001f;
    const float eye[3] = {me.origin[0], me.origin[1], me.origin[2] + me.viewHeightCurrent};
    const uint32_t quietBit = QuietPerkBit();
    const uint32_t jammerBit = JammerPerkBit();
    int noticed = 0;
    // A UAV sweep on the bot's compass this tick.
    bool sweep = false;
    if (me.radarEnabled && now - s_radarSweptAt[clientNum] >= static_cast<int>(params.radarSweepSeconds * 1000.0f))
    {
        sweep = true;
        s_radarSweptAt[clientNum] = now;
    }

    for (int entity = 0; entity < kMaxPlayers && entity < sv_maxclients->current.integer; ++entity)
    {
        if (entity == clientNum)
            continue;
        const gclient_s *other = PlayingClient(entity);
        if (!other || !Opponents(myTeam, other->sess.cs.team))
        {
            perception.Forget(entity);
            continue;
        }
        const playerState_s &ps = other->ps;

        // Hearing and the compass first: they need no line of sight.
        const float speed = GroundSpeed(ps);
        const kb::Stance stance = StanceOf(ps);
        const float footsteps = kb::FootstepRange(speed, stance, (ps.pm_flags & PMF_SPRINTING) != 0,
                                                  quietBit && (ps.perks & quietBit), params);
        if (footsteps > 0.0f && Distance(eye, ps.origin) < footsteps)
            HearAt(perception, entity, eye, ps.origin, params.hearingError, now, rng);
        if ((ps.eFlags & kCompassPingFlag) || (sweep && !(jammerBit && (ps.perks & jammerBit))))
            HearAt(perception, entity, eye, ps.origin, params.compassError, now, rng);

        // Sight.
        const float vh = ps.viewHeightCurrent;
        const float points[3][3] = {
            {ps.origin[0], ps.origin[1], ps.origin[2] + vh},
            {ps.origin[0], ps.origin[1], ps.origin[2] + vh * 0.7f},
            {ps.origin[0], ps.origin[1], ps.origin[2] + vh * 0.4f},
        };
        const float *chest = points[1];
        kb::SightSample sample;
        sample.distance = Distance(eye, chest);
        const float dx = chest[0] - eye[0];
        const float dy = chest[1] - eye[1];
        const float dz = chest[2] - eye[2];
        const float yawTo = std::atan2(dy, dx) * (180.0f / 3.14159265358979f);
        const float pitchTo = -std::atan2(dz, std::hypot(dx, dy)) * (180.0f / 3.14159265358979f);
        sample.yawOff = kb::AngleDelta(yawTo, me.viewangles[1]);
        sample.pitchOff = kb::AngleDelta(pitchTo, me.viewangles[0]);
        sample.speed = speed;
        sample.stance = stance;

        if (!kb::OnScreen(sample, params) || sample.distance >= params.maxRange || !SV_inSnapshot(eye, entity))
        {
            perception.Unseen(entity, now, seconds, params);
            continue;
        }
        int clear = 0;
        for (const float *point : points)
        {
            if (kb::SmokeHides(eye, point, s_smokes, SmokeCount(), now, params))
                continue;
            int hit = 0;
            SV_SightTrace(&hit, eye, vec3_origin, vec3_origin, point, clientNum, entity, MASK_AIMTARGET_VISIBILITY);
            if (!hit)
                ++clear;
        }
        sample.exposure = static_cast<float>(clear) / 3.0f;
        if (perception.See(entity, sample, chest, now, seconds, params) && notices && noticed < maxNotices)
        {
            BotNotice &notice = notices[noticed++];
            notice.entity = entity;
            notice.afterMs = perception.contacts[entity].noticedAfterMs;
            notice.distance = sample.distance;
            notice.offDegrees = std::fmax(std::fabs(sample.yawOff), std::fabs(sample.pitchOff));
            notice.exposure = sample.exposure;
        }
    }

    // Shots since the last tick, from opponents only.
    const int oldest = s_shotCount > kShotRing ? s_shotCount - kShotRing : 0;
    for (int i = oldest; i < s_shotCount; ++i)
    {
        const Shot &shot = s_shots[i % kShotRing];
        if (shot.time <= lastSense || shot.time > now || shot.entity == clientNum)
            continue;
        const gclient_s *other = PlayingClient(shot.entity);
        if (!other || !Opponents(myTeam, other->sess.cs.team))
            continue;
        const float range = shot.silenced ? params.silencedShotHeard : params.shotHeard;
        if (Distance(eye, shot.origin) < range)
            HearAt(perception, shot.entity, eye, shot.origin, params.hearingError * 0.5f, now, rng);
    }
    return noticed;
}
