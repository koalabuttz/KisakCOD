// What a KisakCOD bot knows about other players: only what a player in its
// place could know (docs/bots.md).
//
// The brain never reads another player's position straight from the server
// to decide anything. Each sense tick, src/bots/bot_senses.cpp measures what
// the bot's own eyes and ears would get, and this file turns that into
// knowledge:
//
//   sight     a player on the bot's screen, with a clear line from the bot's
//             eye to their head, chest or hips, fills a noticing meter. It
//             fills fastest for a moving player close to the middle of the
//             screen, slower for one far away, near the edge, crouched,
//             prone or partly hidden. Nobody is noticed in less than
//             minNoticeSeconds of being in view: that is the floor of a
//             human's reaction.
//   hearing   footsteps and gunfire give a rough place, as a player hears
//             them; they turn the bot's head, not its aim.
//   compass   a player the HUD shows as a red dot gives a place as the
//             compass shows it.
//   memory    a noticed player who goes out of sight stays where the bot
//             last saw them, with confidence fading, and is forgotten after
//             forgetSeconds.
//
// No engine call and no engine type: scripts/tests/test_bot_perception.py
// runs it on the host.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace kisak::bots
{

constexpr int kMaxContacts = 64;
// The longest one sense tick may count as looking, in seconds.
constexpr float kMaxSightTick = 0.25f;

struct PerceptionParams
{
    // Seconds to notice a moving, fully exposed player close by in the
    // middle of the screen, and the fastest anyone is ever noticed.
    float noticeSeconds = 0.3f;
    float minNoticeSeconds = 0.2f;

    // Full rate within closeRange; beyond it the rate falls as
    // closeRange / distance, and nothing past maxRange is seen at all (the
    // map's fog also stops sight, before the bot gets here).
    float closeRange = 600.0f;
    float maxRange = 8000.0f;

    // The screen: a player more than this far off the view's centre, across
    // or up and down, is not on it (cg_fov 65 on a 16:9 screen shows about
    // 80 degrees across and 48 up and down). Full rate within focusDegrees
    // of the centre, falling to edgeRate at the screen's edge.
    float screenHalfYaw = 40.0f;
    float screenHalfPitch = 24.0f;
    float focusDegrees = 10.0f;
    float edgeRate = 0.3f;

    // How a half-noticed player fades from the meter when out of sight.
    float awarenessDecayPerSecond = 0.5f;

    // A noticed player out of sight: confidence halves every memorySeconds,
    // and the player is forgotten after forgetSeconds.
    float memorySeconds = 3.0f;
    float forgetSeconds = 10.0f;

    // Hearing, in game units: footsteps of a walking, running and sprinting
    // player (crouched and prone are silent), and a shot.
    float walkHeard = 250.0f;
    float runHeard = 500.0f;
    float sprintHeard = 800.0f;
    float shotHeard = 4000.0f;
    float silencedShotHeard = 300.0f;
    // A heard place is off by up to this fraction of its distance.
    float hearingError = 0.15f;
    // A sound heard is acted on (looked toward) for this long.
    float heardSeconds = 2.0f;
};

enum class Stance : uint8_t
{
    Standing,
    Crouched,
    Prone,
};

// What the bot's eyes get of one player on one sense tick.
struct SightSample
{
    float distance = 0.0f;
    float yawOff = 0.0f;   // degrees off the view's centre, across
    float pitchOff = 0.0f; // and up or down
    float exposure = 0.0f; // the fraction of head, chest and hips in clear view
    float speed = 0.0f;    // across the ground, units a second
    Stance stance = Stance::Standing;
};

inline bool OnScreen(const SightSample &sample, const PerceptionParams &params)
{
    return std::fabs(sample.yawOff) <= params.screenHalfYaw && std::fabs(sample.pitchOff) <= params.screenHalfPitch;
}

// How fast the noticing meter fills (1 = noticed), per second.
inline float NoticeRate(const SightSample &sample, const PerceptionParams &params)
{
    if (!(sample.exposure > 0.0f) || !OnScreen(sample, params) || !(sample.distance < params.maxRange))
        return 0.0f;

    float rate = 1.0f / params.noticeSeconds;

    if (sample.distance > params.closeRange)
        rate *= params.closeRange / sample.distance;

    // Off the centre: the larger of across and up-down, as a share of the
    // way to the screen's edge.
    const float across = std::fabs(sample.yawOff) / params.screenHalfYaw;
    const float upDown = std::fabs(sample.pitchOff) / params.screenHalfPitch;
    const float edge = across > upDown ? across : upDown;
    const float focus = params.focusDegrees / params.screenHalfYaw;
    if (edge > focus)
    {
        const float t = (edge - focus) / (1.0f - focus);
        rate *= 1.0f + (params.edgeRate - 1.0f) * (t > 1.0f ? 1.0f : t);
    }

    rate *= sample.exposure;

    // Movement draws the eye; a still player is half as easy to pick out.
    if (sample.speed < 10.0f)
        rate *= 0.5f;
    else if (sample.speed > 200.0f)
        rate *= 1.25f;

    if (sample.stance == Stance::Crouched)
        rate *= 0.75f;
    else if (sample.stance == Stance::Prone)
        rate *= 0.5f;

    const float fastest = 1.0f / params.minNoticeSeconds;
    return rate > fastest ? fastest : rate;
}

// How far footsteps carry, 0 for none.
inline float FootstepRange(float speed, Stance stance, bool sprinting, bool quiet, const PerceptionParams &params)
{
    if (quiet || stance != Stance::Standing || speed < 60.0f)
        return 0.0f;
    if (sprinting)
        return params.sprintHeard;
    return speed > 150.0f ? params.runHeard : params.walkHeard;
}

struct Contact
{
    bool active = false;
    bool visible = false;   // in clear view on the last sense tick
    bool noticed = false;
    float awareness = 0.0f; // the noticing meter, 0 to 1
    int inViewMs = 0;       // time in view since the meter was last empty
    int noticedAt = 0;
    int noticedAfterMs = 0; // inViewMs when noticed, for the fairness audit
    int lastSeenAt = 0;
    int lastHeardAt = -1;
    float lastSeen[3] = {0.0f, 0.0f, 0.0f};
    float lastHeard[3] = {0.0f, 0.0f, 0.0f};
};

// Where a bot's attention goes, best first.
enum class FocusKind : uint8_t
{
    None,
    Seen,       // a noticed player in view now
    Remembered, // a noticed player out of sight, last seen here
    Heard,      // a recent sound or compass dot
};

struct Focus
{
    FocusKind kind = FocusKind::None;
    int entity = -1;
    float position[3] = {0.0f, 0.0f, 0.0f};
};

class Perception
{
public:
    Contact contacts[kMaxContacts];

    void Reset()
    {
        for (Contact &contact : contacts)
            contact = Contact();
    }

    // A player the bot could see on this tick, `seconds` after the last one.
    // Returns true when the player is noticed on this call.
    bool See(int entity, const SightSample &sample, const float *position, int nowMs, float seconds,
             const PerceptionParams &params)
    {
        if (!Valid(entity))
            return false;
        Contact &contact = contacts[entity];
        // A long gap between ticks (a hitch) is not a long look.
        if (seconds > kMaxSightTick)
            seconds = kMaxSightTick;
        const float rate = NoticeRate(sample, params);
        if (!(rate > 0.0f))
        {
            Unseen(entity, nowMs, seconds, params);
            return false;
        }
        contact.active = true;
        contact.visible = true;
        contact.inViewMs += static_cast<int>(std::lround(seconds * 1000.0f));
        if (contact.noticed)
        {
            Remember(contact, position, nowMs);
            return false;
        }
        contact.awareness += rate * seconds;
        if (contact.awareness < 1.0f)
            return false;
        contact.awareness = 1.0f;
        contact.noticed = true;
        contact.noticedAt = nowMs;
        contact.noticedAfterMs = contact.inViewMs;
        Remember(contact, position, nowMs);
        return true;
    }

    // A player the bot could not see on this tick.
    void Unseen(int entity, int nowMs, float seconds, const PerceptionParams &params)
    {
        if (!Valid(entity))
            return;
        Contact &contact = contacts[entity];
        if (!contact.active)
            return;
        contact.visible = false;
        if (contact.noticed)
        {
            if (nowMs - contact.lastSeenAt > static_cast<int>(params.forgetSeconds * 1000.0f))
                Forget(entity);
            return;
        }
        contact.awareness -= params.awarenessDecayPerSecond * seconds;
        if (contact.awareness <= 0.0f)
        {
            contact.awareness = 0.0f;
            contact.inViewMs = 0;
            if (contact.lastHeardAt < 0 || nowMs - contact.lastHeardAt > static_cast<int>(params.heardSeconds * 1000.0f))
                contact.active = false;
        }
    }

    // A sound (or a compass dot) from a player, heard at `place`: already
    // as wrong as the ear makes it.
    void Hear(int entity, const float *place, int nowMs)
    {
        if (!Valid(entity))
            return;
        Contact &contact = contacts[entity];
        contact.active = true;
        contact.lastHeardAt = nowMs;
        std::memcpy(contact.lastHeard, place, sizeof(contact.lastHeard));
    }

    // The player left the game, died or changed side.
    void Forget(int entity)
    {
        if (Valid(entity))
            contacts[entity] = Contact();
    }

    // How sure the bot is a remembered player is still where it saw them.
    float Confidence(int entity, int nowMs, const PerceptionParams &params) const
    {
        if (!Valid(entity) || !contacts[entity].noticed)
            return 0.0f;
        if (contacts[entity].visible)
            return 1.0f;
        const float seconds = static_cast<float>(nowMs - contacts[entity].lastSeenAt) * 0.001f;
        return std::exp2(-seconds / params.memorySeconds);
    }

    // What to look at: the nearest noticed player in view, else the freshest
    // memory of one, else the latest sound.
    Focus Choose(const float *eye, int nowMs, const PerceptionParams &params) const
    {
        Focus best;
        float bestScore = 0.0f;
        for (int entity = 0; entity < kMaxContacts; ++entity)
        {
            const Contact &contact = contacts[entity];
            if (!contact.active)
                continue;
            FocusKind kind = FocusKind::None;
            float score = 0.0f;
            const float *place = nullptr;
            if (contact.noticed && contact.visible)
            {
                kind = FocusKind::Seen;
                place = contact.lastSeen;
                score = 3.0f + 1.0f / (1.0f + Distance(eye, place));
            }
            else if (contact.noticed)
            {
                kind = FocusKind::Remembered;
                place = contact.lastSeen;
                score = 2.0f * Confidence(entity, nowMs, params);
            }
            if (contact.lastHeardAt >= 0 && nowMs - contact.lastHeardAt <= static_cast<int>(params.heardSeconds * 1000.0f))
            {
                const float heardScore =
                    1.0f + 0.5f * (1.0f - static_cast<float>(nowMs - contact.lastHeardAt) / (params.heardSeconds * 1000.0f));
                if (kind == FocusKind::None || (kind == FocusKind::Remembered && contact.lastHeardAt > contact.lastSeenAt))
                {
                    kind = FocusKind::Heard;
                    place = contact.lastHeard;
                    score = heardScore;
                }
            }
            if (kind == FocusKind::None || score <= bestScore)
                continue;
            bestScore = score;
            best.kind = kind;
            best.entity = entity;
            std::memcpy(best.position, place, sizeof(best.position));
        }
        return best;
    }

private:
    static bool Valid(int entity)
    {
        return entity >= 0 && entity < kMaxContacts;
    }

    static float Distance(const float *a, const float *b)
    {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    static void Remember(Contact &contact, const float *position, int nowMs)
    {
        contact.lastSeenAt = nowMs;
        std::memcpy(contact.lastSeen, position, sizeof(contact.lastSeen));
    }
};

// Where a sound at `source` seems to come from to a listener at `listener`:
// off by up to `error` of the distance across the ground, in a direction
// picked from `unit`, a number in [0, 1).
inline void HeardPlace(const float *listener, const float *source, float error, float unitA, float unitB, float *out)
{
    const float dx = source[0] - listener[0];
    const float dy = source[1] - listener[1];
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float angle = unitA * 6.28318530718f;
    const float off = distance * error * unitB;
    out[0] = source[0] + std::cos(angle) * off;
    out[1] = source[1] + std::sin(angle) * off;
    out[2] = source[2];
}

} // namespace kisak::bots
