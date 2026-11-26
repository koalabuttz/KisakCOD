// What a KisakCOD bot plays with (docs/bots.md, phase 4d): the class it picks
// and the killstreaks it calls in.
//
// A class is picked as a player picks one: from the stock five, by the map
// (a sniper on a big open map, close range on a small one) and the bot's
// leanings, rarely a second sniper when the team has its share, and kept
// from life to life unless it keeps dying fast. The bot sends the class menu's
// answer, as the class menu on a player's screen does ("mr", Cmd_MenuResponse_f).
//
// A killstreak sits in action slot 4 as a weapon (the stock _hardpoints.gsc).
// The bot takes it out a moment after it earns it, when no fight is on, the
// way a player presses 4: a UAV when its side has none up, a helicopter
// straight away, an airstrike once it knows where enemies are. For the
// airstrike's map it picks the place where it saw or heard the most enemies
// lately, away from itself and its teammates, and points at it as the cursor
// on a player's map does (usercmd_s selectedLocation).
//
// No engine call and no engine type: scripts/tests/test_bot_team.py runs it
// on the host.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace kisak::bots
{

// ---------------------------------------------------------------------------
// The class
// ---------------------------------------------------------------------------

enum class PlayerClass : uint8_t
{
    Assault,
    SpecOps,
    HeavyGunner,
    Demolitions,
    Sniper,
    Count,
};

constexpr int kClassCount = static_cast<int>(PlayerClass::Count);

// The class menu's answer for a class (the keys of the stock level.classMap).
inline const char *ClassResponse(PlayerClass c)
{
    switch (c)
    {
    case PlayerClass::Assault: return "assault_mp";
    case PlayerClass::SpecOps: return "specops_mp";
    case PlayerClass::HeavyGunner: return "heavygunner_mp";
    case PlayerClass::Demolitions: return "demolitions_mp";
    case PlayerClass::Sniper: return "sniper_mp";
    default: return "assault_mp";
    }
}

inline const char *ClassName(PlayerClass c)
{
    switch (c)
    {
    case PlayerClass::Assault: return "assault";
    case PlayerClass::SpecOps: return "spec ops";
    case PlayerClass::HeavyGunner: return "heavy gunner";
    case PlayerClass::Demolitions: return "demolitions";
    case PlayerClass::Sniper: return "sniper";
    default: return "none";
    }
}

// How open a map is, 0 for one the size of Shipment or Killhouse to 1 for one
// the size of Overgrown or Pipeline: from the size of the area its minimap
// covers (the compass map, as a player sees it), in units.
inline float MapOpenness(float width, float height)
{
    constexpr float kSmall = 3500.0f;
    constexpr float kLarge = 7500.0f;
    const float across = std::sqrt(width * width + height * height);
    if (!(across > kSmall))
        return 0.0f;
    if (across >= kLarge)
        return 1.0f;
    return (across - kSmall) / (kLarge - kSmall);
}

struct ClassWeights
{
    float weight[kClassCount] = {};
};

// How much a bot leans to each class on a map of `openness`, with its own
// `aggression` and `patience` (0 to 1, bot_mind.h): the bold rush with
// shotguns and SMGs, the patient hold with a sniper rifle or a machine gun.
inline ClassWeights ClassLeanings(float openness, float aggression, float patience)
{
    // Close maps: SMGs and rifles, hardly a sniper. Open maps: a third snipe.
    constexpr float kClose[kClassCount] = {35.0f, 33.0f, 15.0f, 15.0f, 2.0f};
    constexpr float kOpen[kClassCount] = {32.0f, 13.0f, 15.0f, 8.0f, 32.0f};
    ClassWeights w;
    for (int i = 0; i < kClassCount; ++i)
        w.weight[i] = kClose[i] + (kOpen[i] - kClose[i]) * openness;
    w.weight[static_cast<int>(PlayerClass::SpecOps)] *= 0.5f + aggression;
    w.weight[static_cast<int>(PlayerClass::Demolitions)] *= 0.6f + 0.8f * aggression;
    w.weight[static_cast<int>(PlayerClass::Sniper)] *= 0.4f + 1.2f * patience;
    w.weight[static_cast<int>(PlayerClass::HeavyGunner)] *= 0.7f + 0.6f * patience;
    return w;
}

// The most snipers a side of `teamSize` players wants: one in four, at least
// one.
inline int SniperShare(int teamSize)
{
    const int share = teamSize / 4;
    return share < 1 ? 1 : share;
}

// A class by the leanings, `unit` in [0, 1): no sniper when the team already
// has `snipers` of its share, and never `avoid` (PlayerClass::Count for none),
// the class the bot is leaving.
inline PlayerClass PickClass(const ClassWeights &leanings, float unit, int snipers, int teamSize,
                             PlayerClass avoid = PlayerClass::Count)
{
    ClassWeights w = leanings;
    if (snipers >= SniperShare(teamSize))
        w.weight[static_cast<int>(PlayerClass::Sniper)] = 0.0f;
    if (avoid != PlayerClass::Count)
        w.weight[static_cast<int>(avoid)] = 0.0f;
    float total = 0.0f;
    for (float weight : w.weight)
        total += weight > 0.0f ? weight : 0.0f;
    if (!(total > 0.0f))
        return avoid == PlayerClass::Assault ? PlayerClass::SpecOps : PlayerClass::Assault;
    float pick = unit * total;
    for (int i = 0; i < kClassCount; ++i)
    {
        const float weight = w.weight[i] > 0.0f ? w.weight[i] : 0.0f;
        if (pick < weight)
            return static_cast<PlayerClass>(i);
        pick -= weight;
    }
    for (int i = kClassCount - 1; i >= 0; --i)
        if (w.weight[i] > 0.0f)
            return static_cast<PlayerClass>(i);
    return PlayerClass::Assault;
}

// A life shorter than this is one that went badly.
constexpr int kShortLifeMs = 20000;

// Whether a bot that just died thinks about another class, `unit` in [0, 1):
// now and then anyway, and much more often after three short lives in a row.
inline bool RethinkClass(int shortLivesInARow, float unit)
{
    float chance = 0.08f;
    if (shortLivesInARow >= 3)
        chance += 0.3f;
    return unit < chance;
}

// ---------------------------------------------------------------------------
// Killstreaks
// ---------------------------------------------------------------------------

enum class StreakKind : uint8_t
{
    None,
    Radar,      // the UAV: enemies on the side's compass for a while
    Airstrike,  // needs a place on the map
    Helicopter, // flies over the map and hunts
    Other,      // a mod's: taken out like the helicopter
};

inline bool NameHas(const char *name, const char *part)
{
    return name && std::strstr(name, part) != nullptr;
}

// The kind of killstreak from its weapon's name ("radar_mp", "airstrike_mp",
// "helicopter_mp").
inline StreakKind StreakKindFromWeapon(const char *name)
{
    if (!name || !name[0])
        return StreakKind::None;
    if (NameHas(name, "radar") || NameHas(name, "uav"))
        return StreakKind::Radar;
    if (NameHas(name, "airstrike") || NameHas(name, "artillery"))
        return StreakKind::Airstrike;
    if (NameHas(name, "helicopter") || NameHas(name, "chopper"))
        return StreakKind::Helicopter;
    return StreakKind::Other;
}

inline const char *StreakName(StreakKind kind)
{
    switch (kind)
    {
    case StreakKind::Radar: return "UAV";
    case StreakKind::Airstrike: return "airstrike";
    case StreakKind::Helicopter: return "helicopter";
    case StreakKind::Other: return "killstreak";
    default: return "none";
    }
}

// How long after earning a killstreak a bot takes it out, `unit` in [0, 1):
// a moment to notice the HUD's "press 4", finish a reload, look round.
inline int StreakDelayMs(float unit)
{
    return 1500 + static_cast<int>(unit * 3500.0f);
}

// A place the bot thinks an enemy is, `ageMs` ago.
struct EnemyMark
{
    float place[3] = {0.0f, 0.0f, 0.0f};
    int ageMs = 0;
};

struct StrikeParams
{
    float radius = 450.0f;        // enemies this close together count as one target
    float clearOfSelf = 900.0f;   // never this close to the bot itself
    float clearOfFriend = 650.0f; // nor to a teammate
    int maxAgeMs = 12000;         // older marks are worth nothing
};

struct StrikePick
{
    bool found = false;
    float place[3] = {0.0f, 0.0f, 0.0f};
    int enemies = 0;
    float score = 0.0f;
};

inline float PlanarDistance(const float *a, const float *b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

// Where to call an airstrike: around each mark, the fresher marks within
// `radius` count (a fresh one 1, one `maxAgeMs` old nothing), and the place is
// their middle. The best place with at least `minEnemies` marks, clear of the
// bot at `self` and of every teammate in `friends`.
inline StrikePick PickStrike(const EnemyMark *marks, int count, const float (*friends)[3], int friendCount,
                             const float *self, int minEnemies, const StrikeParams &p = StrikeParams())
{
    StrikePick best;
    for (int i = 0; i < count; ++i)
    {
        if (marks[i].ageMs > p.maxAgeMs)
            continue;
        float middle[3] = {0.0f, 0.0f, 0.0f};
        float score = 0.0f;
        int enemies = 0;
        for (int j = 0; j < count; ++j)
        {
            if (marks[j].ageMs > p.maxAgeMs || PlanarDistance(marks[i].place, marks[j].place) > p.radius)
                continue;
            score += 1.0f - static_cast<float>(marks[j].ageMs) / static_cast<float>(p.maxAgeMs);
            for (int k = 0; k < 3; ++k)
                middle[k] += marks[j].place[k];
            ++enemies;
        }
        if (enemies < minEnemies || enemies <= 0)
            continue;
        for (int k = 0; k < 3; ++k)
            middle[k] /= static_cast<float>(enemies);
        if (self && PlanarDistance(middle, self) < p.clearOfSelf)
            continue;
        bool friendNear = false;
        for (int f = 0; f < friendCount && !friendNear; ++f)
            friendNear = PlanarDistance(middle, friends[f]) < p.clearOfFriend;
        if (friendNear)
            continue;
        if (best.found && score <= best.score)
            continue;
        best.found = true;
        best.score = score;
        best.enemies = enemies;
        std::memcpy(best.place, middle, sizeof(best.place));
    }
    return best;
}

// The compass map as the server keeps it (level.compassMapUpperLeft,
// compassNorth, compassMapWorldSize; setMiniMap sets them).
struct CompassFrame
{
    float upperLeft[2] = {0.0f, 0.0f};
    float north[2] = {0.0f, 1.0f};
    float worldSize[2] = {0.0f, 0.0f};
};

// The place on the ground the server reads from a confirmed location
// (ClientThink_real, g_active_mp.cpp), for `selected` as the bytes of
// usercmd_s selectedLocation.
inline void CompassPlace(const CompassFrame &frame, const int8_t selected[2], float out[2])
{
    const float a = (static_cast<float>(selected[0]) + 128.0f) / 255.0f * frame.worldSize[0];
    const float b = (static_cast<float>(selected[1]) + 128.0f) / 255.0f * frame.worldSize[1];
    out[0] = a * frame.north[1] + frame.upperLeft[0] - b * frame.north[0];
    out[1] = frame.upperLeft[1] - a * frame.north[0] - b * frame.north[1];
}

// The bytes of usercmd_s selectedLocation for the cursor on `place`, as a
// player's client sends them (CL_GetSelectedLocation: the cursor at 0 to 1
// across the map, times 255, plus 128). False when the map has no size or the
// place is off it (the cursor then stops at the edge).
inline bool CompassLocation(const CompassFrame &frame, const float *place, int8_t out[2])
{
    out[0] = out[1] = 0;
    if (!(frame.worldSize[0] > 0.0f) || !(frame.worldSize[1] > 0.0f))
        return false;
    const float d0 = place[0] - frame.upperLeft[0];
    const float d1 = place[1] - frame.upperLeft[1];
    const float a = d0 * frame.north[1] - d1 * frame.north[0];
    const float b = -d0 * frame.north[0] - d1 * frame.north[1];
    float across[2] = {a / frame.worldSize[0], b / frame.worldSize[1]};
    bool onMap = true;
    for (int k = 0; k < 2; ++k)
    {
        if (across[k] < 0.0f || across[k] > 1.0f)
        {
            onMap = false;
            across[k] = across[k] < 0.0f ? 0.0f : 1.0f;
        }
        const int step = static_cast<int>(std::lround(across[k] * 255.0f));
        out[k] = static_cast<int8_t>(step - 128);
    }
    return onMap;
}

} // namespace kisak::bots
