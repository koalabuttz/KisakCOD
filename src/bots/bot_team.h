// How KisakCOD's bots play as a team (docs/bots.md, phase 4b).
//
// A team of players talks: "two at the B stairs", "I'll hold long", "going
// for the flag". The bots do the same through a TeamBoard, one per team:
//
//   callouts  a bot that sees an enemy calls out where, once in a while per
//             enemy. Its teammates hear it after the moment it takes to say
//             (heardAtMs), at a place rounded to the callout grid (a callout
//             names an area, not a point), and only for a while.
//   roles     the commander (AssignRoles) gives each bot a part to play from
//             the team's size, the objectives on the compass and the bots'
//             personalities: push (go where the team saw enemies), anchor
//             (hold a spot that overlooks them), roam, attack (go to an
//             objective the compass asks the team to take) and defend (hold a
//             spot that overlooks one it asks the team to hold).
//
// Objectives come from the compass, as a player reads them: the icon's
// material name says whether the team is to take the place or hold it
// (ObjectiveKindFromIcon). In free-for-all every bot is its own team, with no
// board.
//
// No engine call and no engine type: scripts/tests/test_bot_team.py runs it
// on the host.

#pragma once

#include "bot_mind.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace kisak::bots
{

enum class Role : uint8_t
{
    Roam,
    Push,
    Anchor,
    Attack,
    Defend,
};

inline const char *RoleName(Role role)
{
    switch (role)
    {
    case Role::Roam:
        return "roam";
    case Role::Push:
        return "push";
    case Role::Anchor:
        return "anchor";
    case Role::Attack:
        return "attack";
    case Role::Defend:
        return "defend";
    }
    return "?";
}

// What an objective on the compass asks of the team.
enum class ObjectiveKind : uint8_t
{
    None, // not an objective the bots play (a killstreak marker, say)
    Take, // go there: capture, plant, destroy, defuse, pick up
    Hold, // keep the enemy from it
};

// From the material name of a compass icon: the stock gametypes mark
// objectives with compass_waypoint_* icons, "defend" for the team's own and
// the rest (capture, target, bomb, defuse...) for one to go to.
inline ObjectiveKind ObjectiveKindFromIcon(const char *material)
{
    static const char kPrefix[] = "compass_waypoint_";
    if (!material || std::strncmp(material, kPrefix, sizeof(kPrefix) - 1) != 0)
        return ObjectiveKind::None;
    if (std::strstr(material, "defend"))
        return ObjectiveKind::Hold;
    return ObjectiveKind::Take;
}

// A place said out loud: rounded to the callout grid.
constexpr float kCalloutGrid = 128.0f;

inline void CalloutPlace(const float *seen, float *out)
{
    out[0] = (std::floor(seen[0] / kCalloutGrid) + 0.5f) * kCalloutGrid;
    out[1] = (std::floor(seen[1] / kCalloutGrid) + 0.5f) * kCalloutGrid;
    out[2] = seen[2];
}

struct Callout
{
    float place[3] = {0.0f, 0.0f, 0.0f};
    int heardAtMs = 0;
    int enemy = -1;
    bool active = false;
};

class TeamBoard
{
public:
    static constexpr int kMaxCallouts = 24;
    // How long a callout is worth acting on.
    static constexpr int kLifeMs = 15000;
    // A callout of the same enemy this close to an older one replaces it.
    static constexpr float kSamePlace = 400.0f;

    void Reset()
    {
        for (Callout &call : m_calls)
            call = Callout();
        m_next = 0;
    }

    // A teammate calls out `enemy` at `seen` (where they saw them); the team
    // hears it at `heardAtMs`.
    void Report(int enemy, const float *seen, int heardAtMs)
    {
        float place[3];
        CalloutPlace(seen, place);
        Callout *slot = nullptr;
        for (Callout &call : m_calls)
        {
            if (call.active && call.enemy == enemy && Distance(call.place, place) < kSamePlace)
            {
                slot = &call;
                break;
            }
        }
        if (!slot)
        {
            slot = &m_calls[m_next];
            m_next = (m_next + 1) % kMaxCallouts;
        }
        std::memcpy(slot->place, place, sizeof(place));
        slot->heardAtMs = heardAtMs;
        slot->enemy = enemy;
        slot->active = true;
    }

    // Whether a callout is heard by `nowMs` and no older than `maxAgeMs`.
    static bool Heard(const Callout &call, int nowMs, int maxAgeMs)
    {
        return call.active && call.heardAtMs <= nowMs && nowMs - call.heardAtMs <= maxAgeMs;
    }

    // The freshest callout heard by `nowMs`, no older than `maxAgeMs`.
    const Callout *Freshest(int nowMs, int maxAgeMs = kLifeMs) const
    {
        const Callout *best = nullptr;
        for (const Callout &call : m_calls)
            if (Heard(call, nowMs, maxAgeMs) && (!best || call.heardAtMs > best->heardAtMs))
                best = &call;
        return best;
    }

    // The callout heard by `nowMs` nearest `place`, no older than `maxAgeMs`.
    const Callout *Nearest(const float *place, int nowMs, int maxAgeMs = kLifeMs) const
    {
        const Callout *best = nullptr;
        float bestDistance = 0.0f;
        for (const Callout &call : m_calls)
        {
            if (!Heard(call, nowMs, maxAgeMs))
                continue;
            const float distance = Distance(call.place, place);
            if (!best || distance < bestDistance)
            {
                best = &call;
                bestDistance = distance;
            }
        }
        return best;
    }

    const Callout *Calls() const { return m_calls; }

private:
    static float Distance(const float *a, const float *b)
    {
        return std::hypot(a[0] - b[0], a[1] - b[1]);
    }

    Callout m_calls[kMaxCallouts];
    int m_next = 0;
};

// The commander: roles for a team's `count` bots (in a fixed order, with
// their personalities), given how many objectives the compass shows to take
// and to hold. The boldest attack and push, the most patient defend and
// anchor, the rest roam. Same input, same roles, so a team keeps its roles
// while nothing changes.
inline void AssignRoles(const Personality *people, int count, int takeObjectives, int holdObjectives, Role *out)
{
    constexpr int kMax = 64;
    if (count <= 0)
        return;
    if (count > kMax)
        count = kMax;

    int attack = 0;
    int defend = 0;
    int push = 0;
    int anchor = 0;
    if (takeObjectives > 0 && holdObjectives > 0)
    {
        attack = (count * 45 + 50) / 100;
        defend = (count * 30 + 50) / 100;
        if (attack < 1)
            attack = 1;
        if (count >= 2 && defend < 1)
            defend = 1;
        push = count - attack - defend;
    }
    else if (takeObjectives > 0)
    {
        attack = (count * 60 + 99) / 100;
        push = count - attack;
    }
    else if (holdObjectives > 0)
    {
        defend = (count + 1) / 2;
        push = count - defend;
    }
    else
    {
        push = (count * 40 + 50) / 100;
        anchor = count >= 3 ? (count * 25 + 50) / 100 : 0;
        if (count >= 3 && anchor < 1)
            anchor = 1;
    }
    if (attack + defend + push + anchor > count)
        push = count - attack - defend - anchor;

    // Order by aggression (boldest first), then pick the patient from the rest.
    int order[kMax];
    bool taken[kMax];
    for (int i = 0; i < count; ++i)
    {
        order[i] = i;
        taken[i] = false;
        out[i] = Role::Roam;
    }
    for (int i = 1; i < count; ++i)
    {
        const int index = order[i];
        int j = i;
        while (j > 0 && people[order[j - 1]].aggression < people[index].aggression)
        {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = index;
    }
    auto takeBoldest = [&](int howMany, Role role) {
        for (int i = 0; i < count && howMany > 0; ++i)
        {
            if (taken[order[i]])
                continue;
            taken[order[i]] = true;
            out[order[i]] = role;
            --howMany;
        }
    };
    auto takeMostPatient = [&](int howMany, Role role) {
        while (howMany-- > 0)
        {
            int best = -1;
            for (int i = 0; i < count; ++i)
                if (!taken[i] && (best < 0 || people[i].patience > people[best].patience))
                    best = i;
            if (best < 0)
                return;
            taken[best] = true;
            out[best] = role;
        }
    };
    takeBoldest(attack, Role::Attack);
    takeMostPatient(defend, Role::Defend);
    takeMostPatient(anchor, Role::Anchor);
    takeBoldest(push, Role::Push);
}

} // namespace kisak::bots
