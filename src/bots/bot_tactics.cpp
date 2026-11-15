// The bots' map sense (docs/bots.md, phase 4): what a player learns of a
// map's shape by playing it. Everything here traces the map's solid geometry
// only: no other player's place is read, so a bot finds cover from where it
// thinks an enemy is, and pre-aims corners its own eyes can see.

#include <universal/q_shared.h>
#include <server_mp/server_mp.h>
#include <server/sv_world.h>

#include "bot_brain.h"
#include "bot_motor.h"
#include "bot_navmesh.h"

#include <cmath>

namespace
{

namespace kb = kisak::bots;

// Chest and head of a standing player above their feet.
constexpr float kChest = 42.0f;
constexpr float kHead = 60.0f;
// How far a bot looks for cover, and the longest walk to it.
constexpr float kCoverRadius = 450.0f;
constexpr float kCoverMaxWalk = 700.0f;
constexpr int kCoverTries = 12;

float NextUnit(void *rng)
{
    return static_cast<kb::Rng *>(rng)->Unit();
}

} // namespace

bool Bot_ClearLine(const float *from, const float *to, void *)
{
    int hit = 0;
    SV_SightTrace(&hit, from, vec3_origin, vec3_origin, to, ENTITYNUM_NONE, ENTITYNUM_NONE, CONTENTS_SOLID);
    return !hit;
}

bool Bot_HiddenFrom(const float *place, const float *threat)
{
    const float chest[3] = {place[0], place[1], place[2] + kChest};
    const float head[3] = {place[0], place[1], place[2] + kHead};
    return !Bot_ClearLine(threat, chest, nullptr) && !Bot_ClearLine(threat, head, nullptr);
}

bool Bot_FindCover(const float *origin, const float *threat, const kb::NavMesh &nav, kb::Rng &rng, float *out)
{
    float best = kCoverMaxWalk;
    bool found = false;
    float path[48][3];
    for (int attempt = 0; attempt < kCoverTries; ++attempt)
    {
        float place[3];
        if (!nav.RandomPointNear(origin, kCoverRadius, NextUnit, &rng, place))
            continue;
        const float straight = std::hypot(place[0] - origin[0], place[1] - origin[1]);
        if (straight >= best || !Bot_HiddenFrom(place, threat))
            continue;
        const int count = nav.FindPath(origin, place, path, 48);
        if (count < 2)
            continue;
        const float walk = kb::PathLength(path, count);
        const float *end = path[count - 1];
        if (walk >= best || std::hypot(end[0] - place[0], end[1] - place[1]) > 24.0f)
            continue;
        best = walk;
        out[0] = place[0];
        out[1] = place[1];
        out[2] = place[2];
        found = true;
    }
    return found;
}
