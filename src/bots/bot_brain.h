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
}

void Bot_RegisterDvars();

// Builds and runs this frame's user command for a test client when the brain
// is on. Returns false when the brain is off, and the caller then runs the
// retail SV_BotUserMove.
bool Bot_UserMove(client_t *cl);

// The navigation mesh of the map that is loaded, built or read from the home
// folder's cache the first time it is asked for (bot_nav_world.cpp), or
// nullptr when there is none.
const kisak::bots::NavMesh *Bot_Nav();
