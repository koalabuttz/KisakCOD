// KisakCOD's own bot brain for multiplayer test clients (docs/bots.md).
//
// Each server frame SV_UpdateBots asks Bot_UserMove for every active test
// client. With bot_brain 1 the brain builds the bot's user command:
//
//   read      the bot's own player state (view, weapon, alive or dead), as a
//             player's client knows it;
//   decide    what to look at and do (for now: look around while idle, ask to
//             respawn when dead; later phases add perception, navigation,
//             combat and objectives, docs/bots.md);
//   move      the view motor (bot_motor.h) turns toward that at a human pace;
//   send      the usercmd_s goes through SV_ClientThink like a player's.
//
// The brain does not read other players or entities. Later phases add a
// perception step that only reports what this bot could see or hear.

#include <universal/q_shared.h>
#include <server_mp/server_mp.h>
#include <server/sv_game.h>
#include <game_mp/g_main_mp.h>

#include "bot_brain.h"
#include "bot_motor.h"

#include <cstring>

namespace
{

namespace kb = kisak::bots;

const dvar_t *bot_brain;

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
};

BotSlot s_slots[kMaxBotSlots];

kb::ViewMotorParams IdleMotor()
{
    kb::ViewMotorParams params;
    params.responsiveness = 5.0f;
    params.maxSpeed = 120.0f;
    return params;
}

bool IsDead(const playerState_s *ps)
{
    return ps->pm_type == PM_DEAD || ps->pm_type == PM_DEAD_LINKED;
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
    }

    const playerState_s *ps = SV_GameClientNum(clientNum);
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
        kb::PrimeView(bot.view, ps->viewangles[0], ps->viewangles[1]);
        if (newPlace)
        {
            bot.look.pitch = bot.view.angles[0];
            bot.look.yaw = bot.view.angles[1];
            bot.look.holdSeconds = 0.0f;
            bot.lookHeld = 0.0f;
        }
    }

    if (!G_GetClientArchiveTime(clientNum))
    {
        if (IsDead(ps))
        {
            // Tap use now and then: the stock gametypes respawn on it where
            // they wait for a button, and it skips nothing that matters.
            bot.respawnTap = (bot.respawnTap + 1) % 10;
            if (bot.respawnTap == 0)
                cmd.buttons |= BUTTON_USE_RELOAD;
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

    for (int axis = 0; axis < 2; ++axis)
    {
        cmd.angles[axis] = kb::CmdAngleFor(bot.view.angles[axis], ps->delta_angles[axis]);
        bot.sentView[axis] = kb::ViewForCmdAngle(cmd.angles[axis], ps->delta_angles[axis]);
    }
    cmd.angles[2] = kb::CmdAngleFor(0.0f, ps->delta_angles[2]);

    cl->header.deltaMessage = cl->header.netchan.outgoingSequence - 1;
    SV_ClientThink(cl, &cmd);
    return true;
}
