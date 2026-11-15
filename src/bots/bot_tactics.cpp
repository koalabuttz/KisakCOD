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
#include "bot_spots.h"

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

// The spot table: spots kSpotSpacing apart over the mesh, from kSpotTries
// random places tried kSpotTriesPerFrame a frame, then kLinesPerFrame lines
// traced a frame.
constexpr float kSpotSpacing = 200.0f;
constexpr int kSpotTries = 4000;
constexpr int kSpotTriesPerFrame = 100;
constexpr int kLinesPerFrame = 200;

kb::SpotTable s_spots;
float s_spotPlaces[kb::SpotTable::kMaxSpots][3];
int s_spotCount;
int s_spotTries;
int s_spotGeneration = -1;
int s_spotFrame = -1;
uint32_t s_spotStarted;
bool s_spotTracing;
bool s_spotDone;
kb::Rng s_spotRng;

// The walk from `origin` to `place` when it ends there and is shorter than
// `longest`, else a negative number.
float WalkTo(const kb::NavMesh &nav, const float *origin, const float *place, float longest)
{
    float path[48][3];
    const int count = nav.FindPath(origin, place, path, 48);
    if (count < 2)
        return -1.0f;
    const float walk = kb::PathLength(path, count);
    const float *end = path[count - 1];
    if (walk >= longest || std::hypot(end[0] - place[0], end[1] - place[1]) > 24.0f)
        return -1.0f;
    return walk;
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
    // The spots the threat does not see, nearest first: the first that hides
    // a player from the threat itself, a short walk away.
    if (s_spots.Ready())
    {
        int spots[8];
        const int count = s_spots.CoverFrom(origin, threat, kCoverRadius, spots, 8);
        for (int i = 0; i < count; ++i)
        {
            const float *place = s_spots.Place(spots[i]);
            if (Bot_HiddenFrom(place, threat) && WalkTo(nav, origin, place, kCoverMaxWalk) >= 0.0f)
            {
                out[0] = place[0];
                out[1] = place[1];
                out[2] = place[2];
                return true;
            }
        }
    }

    // Places tried at random, the nearest that hides.
    float best = kCoverMaxWalk;
    bool found = false;
    for (int attempt = 0; attempt < kCoverTries; ++attempt)
    {
        float place[3];
        if (!nav.RandomPointNear(origin, kCoverRadius, NextUnit, &rng, place))
            continue;
        const float straight = std::hypot(place[0] - origin[0], place[1] - origin[1]);
        if (straight >= best || !Bot_HiddenFrom(place, threat))
            continue;
        const float walk = WalkTo(nav, origin, place, best);
        if (walk < 0.0f)
            continue;
        best = walk;
        out[0] = place[0];
        out[1] = place[1];
        out[2] = place[2];
        found = true;
    }
    return found;
}

void Bot_StepSpots(const kb::NavMesh &nav, const float *seed)
{
    // Once a server frame, whichever bot asks first.
    if (s_spotFrame == svs.time)
        return;
    s_spotFrame = svs.time;

    const int generation = Bot_NavGeneration();
    if (generation != s_spotGeneration)
    {
        s_spotGeneration = generation;
        s_spots.Clear();
        s_spotCount = 0;
        s_spotTries = 0;
        s_spotTracing = false;
        s_spotDone = false;
        s_spotRng.Seed(0x73706f74u ^ static_cast<uint32_t>(generation));
        s_spotStarted = Sys_Milliseconds();
    }
    if (s_spotDone)
        return;

    if (!s_spotTracing)
    {
        for (int i = 0; i < kSpotTriesPerFrame && s_spotTries < kSpotTries && s_spotCount < kb::SpotTable::kMaxSpots;
             ++i, ++s_spotTries)
        {
            float place[3];
            if (nav.RandomReachablePoint(seed, NextUnit, &s_spotRng, place)
                && kb::FarFromAll(place, s_spotPlaces, s_spotCount, kSpotSpacing))
            {
                s_spotPlaces[s_spotCount][0] = place[0];
                s_spotPlaces[s_spotCount][1] = place[1];
                s_spotPlaces[s_spotCount][2] = place[2];
                ++s_spotCount;
            }
        }
        if (s_spotTries < kSpotTries && s_spotCount < kb::SpotTable::kMaxSpots)
            return;
        s_spots.Begin(s_spotPlaces, s_spotCount);
        s_spotTracing = true;
    }

    if (s_spots.Step(Bot_ClearLine, nullptr, kLinesPerFrame))
    {
        s_spotDone = true;
        Com_Printf(CON_CHANNEL_SERVER, "bots: map spots ready: %d spots, %d lines, in %.1f s\n", s_spots.Count(),
                   s_spots.Lines(), (Sys_Milliseconds() - s_spotStarted) * 0.001f);
    }
}

const kb::SpotTable *Bot_Spots()
{
    return s_spots.Ready() ? &s_spots : nullptr;
}

bool Bot_FindOverwatch(const float *origin, const float *target, const float *around, float radius,
                       const kb::NavMesh &nav, kb::Rng &rng, float *out)
{
    if (!s_spots.Ready())
        return false;
    const float targetFeet[3] = {target[0], target[1], target[2] - kb::SpotTable::kEye};
    int spots[6];
    const int count = s_spots.Overwatch(targetFeet, around, radius, spots, 6);
    if (count <= 0)
        return false;
    // One of the best few, so a team's anchors do not all pick the same.
    const int first = static_cast<int>(rng.Unit() * static_cast<float>(count < 3 ? count : 3));
    for (int i = 0; i < count; ++i)
    {
        const float *place = s_spots.Place(spots[(first + i) % count]);
        const float eye[3] = {place[0], place[1], place[2] + kb::SpotTable::kEye};
        if (!Bot_ClearLine(eye, target, nullptr))
            continue;
        if (WalkTo(nav, origin, place, 4000.0f) < 0.0f)
            continue;
        out[0] = place[0];
        out[1] = place[1];
        out[2] = place[2];
        return true;
    }
    return false;
}
