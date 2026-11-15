// What a KisakCOD bot decides to do, and where it looks while it does it
// (docs/bots.md, phase 4).
//
// Each server frame the brain fills a Situation from what the bot knows (its
// perception, its own player state, the map sense of bot_tactics.cpp) and the
// Mind scores four intents, the way shipped shooter bots weigh their options:
//
//   roam      go somewhere: a place it has not seen for a while, more often
//             one where it heard or saw enemies lately (its AreaMemory);
//             now and then it stops on arrival and holds an angle.
//   hunt      go to where it last saw an enemy, or heard one, or was shot
//             from, expecting them there.
//   fight     an enemy it noticed is in view (bot_combat.h aims and fires);
//             it keeps walking or strafes.
//   cover     reloading, hurt or outnumbered in a fight: get to a place the
//             enemy cannot see or shoot, wait there, then go back in.
//
// The scores depend on the bot's personality (aggression, patience), so two
// bots in the same spot do not always do the same thing. The current intent
// gets a bonus and a short commitment, so a bot does not flip back and forth.
//
// While it walks without an enemy in view, a bot pre-aims: it looks at the
// last point of its way it can still see, at head height, which is the corner
// an enemy would come round (PreAimPoint).
//
// No engine call and no engine type: scripts/tests/test_bot_mind.py runs it on
// the host.

#pragma once

#include "bot_motor.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace kisak::bots
{

enum class Intent : uint8_t
{
    Roam,
    Hunt,
    Fight,
    Cover,
};

inline const char *IntentName(Intent intent)
{
    switch (intent)
    {
    case Intent::Roam:
        return "roam";
    case Intent::Hunt:
        return "hunt";
    case Intent::Fight:
        return "fight";
    case Intent::Cover:
        return "cover";
    }
    return "?";
}

// One bot's leanings, each in [0, 1], drawn when the bot joins.
struct Personality
{
    float aggression = 0.5f; // pushes and hunts rather than holds and hides
    float patience = 0.5f;   // holds angles longer and more often
};

inline Personality RandomPersonality(Rng &rng)
{
    Personality personality;
    personality.aggression = rng.Range(0.2f, 0.9f);
    personality.patience = rng.Range(0.2f, 0.9f);
    return personality;
}

// What the bot knows this frame, as a player in its place would.
struct Situation
{
    int nowMs = 0;
    float health = 1.0f; // of its maximum
    float clip = 1.0f;   // share of the clip left (1 with no gun)
    bool reloading = false;
    bool canReload = false;      // ammunition left outside the clip
    int seenEnemies = 0;         // noticed opponents in view now
    bool enemyInView = false;    // its focus is one of them
    bool enemyRemembered = false; // a noticed opponent out of sight, not yet forgotten
    float rememberedConfidence = 0.0f;
    bool heardSomething = false; // a recent sound, compass dot or hit from a direction
    int hurtAgoMs = 1000000;     // since it last took damage
    bool coverKnown = false;     // the map sense found cover from the threat
    bool inCover = false;        // and the bot is there
};

struct IntentScores
{
    float roam = 0.0f;
    float hunt = 0.0f;
    float fight = 0.0f;
    float cover = 0.0f;

    float Of(Intent intent) const
    {
        switch (intent)
        {
        case Intent::Roam:
            return roam;
        case Intent::Hunt:
            return hunt;
        case Intent::Fight:
            return fight;
        case Intent::Cover:
            return cover;
        }
        return 0.0f;
    }
};

// How much the bot wants to get out of the fight: an empty or reloading clip,
// low health after a hit, or more than one enemy in view. 0 to 1.
inline float CoverNeed(const Situation &s, const Personality &p)
{
    float need = 0.0f;
    if ((s.reloading || s.clip < 0.25f) && s.canReload)
        need = s.reloading ? 1.0f : 0.8f;
    if (s.health < 0.5f && s.hurtAgoMs < 4000)
        need = std::fmax(need, 0.6f + 1.6f * (0.5f - s.health));
    if (s.seenEnemies >= 2)
        need = std::fmax(need, 0.6f + 0.25f * static_cast<float>(s.seenEnemies - 2));
    // Health comes back after a few seconds out of fire (the stock
    // regeneration); a hurt bot that is safe waits for it.
    if (s.inCover && s.health < 0.8f)
        need = std::fmax(need, 0.6f);
    return std::fmin(need, 1.0f) * (1.0f - 0.5f * p.aggression);
}

inline IntentScores ScoreIntents(const Situation &s, const Personality &p)
{
    IntentScores scores;
    scores.roam = 0.2f;
    if (s.enemyInView)
        scores.fight = 1.0f + 0.2f * p.aggression;
    if (s.coverKnown && (s.enemyInView || s.hurtAgoMs < 3000 || s.inCover))
        scores.cover = 1.4f * CoverNeed(s, p);
    if (!s.enemyInView)
    {
        float hunt = 0.0f;
        if (s.enemyRemembered)
            hunt = 0.3f + 0.5f * s.rememberedConfidence;
        if (s.heardSomething)
            hunt = std::fmax(hunt, 0.45f);
        if (hunt > 0.0f)
            scores.hunt = hunt * (0.6f + 0.8f * p.aggression);
    }
    return scores;
}

class Mind
{
public:
    Intent intent = Intent::Roam;
    int sinceMs = 0;
    bool changed = false; // on the frame Decide picked a new intent

    void Reset(int nowMs)
    {
        intent = Intent::Roam;
        sinceMs = nowMs;
        changed = true;
    }

    // Picks this frame's intent: the best score, with a bonus for the current
    // one, and a moment of commitment before cover or a hunt is dropped.
    Intent Decide(const Situation &s, const Personality &p)
    {
        const IntentScores scores = ScoreIntents(s, p);
        Intent best = intent;
        float bestScore = scores.Of(intent) + kKeepBonus;
        const bool committed = (intent == Intent::Cover || intent == Intent::Hunt) && s.nowMs - sinceMs < kCommitMs
                               && scores.Of(intent) > 0.0f;
        for (Intent option : {Intent::Fight, Intent::Cover, Intent::Hunt, Intent::Roam})
        {
            const float score = scores.Of(option);
            if (score > bestScore && !(committed && option != Intent::Fight && option != Intent::Cover))
            {
                best = option;
                bestScore = score;
            }
        }
        changed = best != intent;
        if (changed)
        {
            intent = best;
            sinceMs = s.nowMs;
        }
        return intent;
    }

    static constexpr float kKeepBonus = 0.1f;
    static constexpr int kCommitMs = 1500;
};

// ---------------------------------------------------------------------------
// Where the bot has been, and where it met enemies: a coarse grid over the
// map, one per bot (what this bot itself saw and heard, not its team's).

class AreaMemory
{
public:
    static constexpr int kCells = 64;
    static constexpr float kCellSize = 256.0f;

    void Reset()
    {
        std::memset(m_visited, 0, sizeof(m_visited));
        std::memset(m_enemy, 0, sizeof(m_enemy));
    }

    void Visit(const float *place, int nowMs)
    {
        m_visited[Cell(place)] = nowMs;
    }

    void EnemyAt(const float *place, int nowMs)
    {
        m_enemy[Cell(place)] = nowMs;
    }

    // How much the bot wants to go to `place`: places it has not seen for a
    // minute, and places with recent enemies (more so for an aggressive bot).
    float Interest(const float *place, int nowMs, const Personality &p) const
    {
        const int cell = Cell(place);
        const float unseen = m_visited[cell] ? std::fmin(static_cast<float>(nowMs - m_visited[cell]) / 60000.0f, 1.0f) : 1.0f;
        float enemy = 0.0f;
        if (m_enemy[cell])
        {
            const float ago = static_cast<float>(nowMs - m_enemy[cell]) / 30000.0f;
            enemy = ago < 1.0f ? 1.0f - ago : 0.0f;
        }
        return unseen + enemy * (0.5f + p.aggression);
    }

private:
    static int Cell(const float *place)
    {
        int x = static_cast<int>(std::floor(place[0] / kCellSize)) + kCells / 2;
        int y = static_cast<int>(std::floor(place[1] / kCellSize)) + kCells / 2;
        x = x < 0 ? 0 : (x >= kCells ? kCells - 1 : x);
        y = y < 0 ? 0 : (y >= kCells ? kCells - 1 : y);
        return y * kCells + x;
    }

    int m_visited[kCells * kCells];
    int m_enemy[kCells * kCells];
};

// ---------------------------------------------------------------------------
// Pre-aiming.

// A clear line between two points through the map, without players: the
// engine's trace in the brain, a made-up wall in the test.
using ClearLine = bool (*)(const float *from, const float *to, void *context);

constexpr float kPreAimStep = 64.0f;
constexpr float kPreAimReach = 960.0f;
constexpr float kHeadHeight = 60.0f;

// The point a walking bot pre-aims: along its way (`corners[index]` onward,
// starting from `origin`), every kPreAimStep units up to kPreAimReach, the
// last one it can see from `eye` at head height, which is where the way turns
// out of sight round a corner. With nothing hidden, the furthest one. Returns
// false when the way is too short to look along.
inline bool PreAimPoint(const float *eye, const float *origin, const float (*corners)[3], int count, int index,
                        ClearLine clear, void *context, float *out)
{
    if (index >= count)
        return false;
    float from[3] = {origin[0], origin[1], origin[2]};
    float travelled = 0.0f;
    float nextSample = kPreAimStep;
    bool found = false;
    for (int corner = index; corner < count && travelled < kPreAimReach; ++corner)
    {
        const float *to = corners[corner];
        const float dx = to[0] - from[0];
        const float dy = to[1] - from[1];
        const float dz = to[2] - from[2];
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        while (length > 0.0f && nextSample <= travelled + length && nextSample <= kPreAimReach)
        {
            const float t = (nextSample - travelled) / length;
            const float sample[3] = {from[0] + dx * t, from[1] + dy * t, from[2] + dz * t + kHeadHeight};
            if (!clear(eye, sample, context))
                return found;
            std::memcpy(out, sample, sizeof(sample));
            found = true;
            nextSample += kPreAimStep;
        }
        travelled += length;
        from[0] = to[0];
        from[1] = to[1];
        from[2] = to[2];
    }
    return found;
}

} // namespace kisak::bots
