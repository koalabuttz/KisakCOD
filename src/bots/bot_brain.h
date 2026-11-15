// KisakCOD's own bot brain for multiplayer test clients (docs/bots.md).
//
// Test clients (addtestclient(), scr_testclients) have always pressed random
// buttons every server frame (SV_BotUserMove). With bot_brain 1 the server
// asks this brain for each bot's user command instead. The brain only ever
// produces a usercmd_s, the same thing a player's client sends, so a bot moves,
// turns and shoots through the game's own player code.
//
// The bots are always compiled in; bot_brain chooses them when a game or a
// server starts (it can also be changed between rounds). 0 keeps the retail
// random test clients, which the existing server tests rely on.

#pragma once

struct client_t;

namespace kisak::bots
{
class NavMesh;
class Perception;
class SpotTable;
struct PerceptionParams;
struct Rng;
}

void Bot_RegisterDvars();

// Builds and runs this frame's user command for a test client when the brain
// is on. Returns false when the brain is off, and the caller then runs the
// retail SV_BotUserMove.
bool Bot_UserMove(client_t *cl);

// The ping the scoreboard shows for a test client the brain drives, or -1
// when the brain does not drive it. A test client acknowledges no packets, so
// SV_CalcPings would give it the retail 999; a bot shows a steady ping of its
// own instead (25 to 75 ms, wobbling a few ms), as a player on a good line.
int Bot_Ping(int clientNum);

// The navigation mesh of the map that is loaded, built or read from the home
// folder's cache the first time it is asked for (bot_nav_world.cpp), or
// nullptr when there is none.
const kisak::bots::NavMesh *Bot_Nav();

// Changes each time Bot_Nav() loads a mesh (a new map, or the same one
// again), for what is worked out from the mesh.
int Bot_NavGeneration();

// A player a bot noticed on a sense tick, for the log and the fairness
// audit: how long the player was in view first, how far and how far off the
// view's centre (degrees), and how much of them showed (0 to 1).
struct BotNotice
{
    int entity = -1;
    int afterMs = 0;
    float distance = 0.0f;
    float offDegrees = 0.0f;
    float exposure = 0.0f;
};

// One sense tick for the bot in `clientNum`, `now` and `lastSense` in server
// milliseconds (bot_senses.cpp): what its eyes and ears get of every other
// player goes into `perception`. Writes the players noticed on this tick to
// `notices` and returns how many.
int Bot_Sense(int clientNum,
              kisak::bots::Perception &perception,
              const kisak::bots::PerceptionParams &params,
              int now,
              int lastSense,
              kisak::bots::Rng &rng,
              BotNotice *notices,
              int maxNotices);

// Every shot the players have fired since the server started, as the bots'
// ears counted them (bot_senses.cpp).
int Bot_ShotsFired();

// The map sense (bot_tactics.cpp): what a player learns of a map's shape by
// playing it, from the map's own geometry and never from other players.

// A clear line between two points through the map's solid geometry (walls,
// floors, closed doors); players and foliage do not count. `context` is
// unused, for kisak::bots::ClearLine.
bool Bot_ClearLine(const float *from, const float *to, void *context);

// A place on the mesh near `origin` (the bot's feet) that hides a player's
// chest and head from `threat` (an eye where the bot thinks an enemy is),
// reachable in a short walk; the nearest of a few tries. False when none is
// found.
bool Bot_FindCover(const float *origin, const float *threat, const kisak::bots::NavMesh &nav,
                   kisak::bots::Rng &rng, float *out);

// Whether a player standing at `place` (feet) has their chest and head hidden
// from `threat`.
bool Bot_HiddenFrom(const float *place, const float *threat);

// The spots of the map and which see each other (bot_spots.h), worked out
// once per map a little each server frame: call Bot_StepSpots once a frame
// with a place on the mesh (a bot's feet) to start and continue it. The
// cover search uses the table once it is ready, and so does Bot_FindOverwatch.
void Bot_StepSpots(const kisak::bots::NavMesh &nav, const float *seed);
const kisak::bots::SpotTable *Bot_Spots();

// A spot within `radius` of `around` from which a standing player sees
// `target` (an eye or a place at chest height), reachable from `origin`; one
// of the most sheltered few, by `rng`. False without the spot table or such a
// spot.
bool Bot_FindOverwatch(const float *origin, const float *target, const float *around, float radius,
                       const kisak::bots::NavMesh &nav, kisak::bots::Rng &rng, float *out);
