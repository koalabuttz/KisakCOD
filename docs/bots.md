# KisakCOD bots

KisakCOD is getting its own multiplayer bots: opponents that play fair and
play like people. The research and the full plan are in the project's shared
folder (`bots/sota-bots-research.md`); this guide says what is in the code
now and how to use it.

## What "fair" means here

- A bot plays through the same door as a player. Its only output is the
  `usercmd_s` a player's client sends (view angles, movement, buttons), which
  the server runs through `SV_ClientThink` and Pmove. It cannot turn faster,
  move differently or shoot differently from a player: recoil, spread, sway,
  aim-down-sight time and damage are the game's own.
- A bot turns at a human pace. The view motor (`src/bots/bot_motor.h`) moves
  the view like a hand moves a mouse or a stick: it starts a turn smoothly, has
  a top speed and slows as it arrives.
- A bot will only know what a player could know (later phases): what it saw in
  its field of view, what it heard, and what its own HUD shows. It does not read
  other players' positions from the server.

## Using them

The bots are always compiled in. `bot_brain` chooses them:

| `bot_brain` | Test clients use |
|---|---|
| `0` (default) | the retail random inputs (`SV_BotUserMove`), as before |
| `1` | KisakCOD's bot brain |

Set it when the game or server starts, for example on a dedicated server:

```
./KisakCOD-dedi +set bot_brain 1 +set scr_testclients 6 +map mp_crash
```

or in a listen server's console before `scr_testclients` / `devmap`. It can be
changed at any time; each bot starts afresh when it does.

`bot_brain` is a setting this port added, so it is on the list a game server
may not set on a player's client (`src/qcommon/server_dvar_guard.h`).

## What the bots do now (phase 0)

The groundwork only: the brain takes over the test clients' commands, stands,
looks around at a human pace (a new direction every 1.5 to 4 seconds), carries
on from wherever the server puts its view (a spawn, a killcam, a script's
`setPlayerAngles`) and taps use while dead. It does not move, aim or shoot yet.

Next phases, in order: a navigation mesh built from each map's collision data
(cached in the home folder, never committed), the perception step (sight,
hearing, HUD), the motor model and combat for TDM and FFA, decisions and
objective modes, then difficulty levels, start-a-game and start-a-server
options, and the PS Vita's bot budget (6v6 TDM first, then 9v9 Ground War).

## Code

| Where | What |
|---|---|
| `src/bots/bot_brain.h`, `bot_brain.cpp` | `bot_brain`, and `Bot_UserMove`: one bot's command for this server frame |
| `src/bots/bot_motor.h` | the view motor, command angle conversion and each bot's random source; no engine types |
| `src/server_mp/sv_main_mp.cpp` | `SV_UpdateBots` asks `Bot_UserMove` first and runs the retail `SV_BotUserMove` when the brain is off |

`delta_angles`: Pmove computes a view as `delta_angles + cmd.angles * 360 /
65536`, and the server changes `delta_angles` on a spawn, a mantle and when
it clamps pitch. The brain therefore sends `CmdAngleFor(view, delta_angles)`
and compares the view the server reports with the view it asked for, to notice
when the server moved it.

The brain sets the command's `serverTime` to the server's time, as a player's
client does; the retail random inputs leave it at zero, which the game clamps
to one second behind.

## Checks

- `bot_motor` (`scripts/tests/test_bot_motor.py`, in `run_linux_checks.py`):
  builds `tests/bots/bot_motor_test.cpp` on the host, 64-bit with sanitizers
  and 32-bit. Command angles round-trip to the view Pmove computes for any
  `delta_angles`; turns take the short way, keep their top speed, never
  overshoot, settle in 0.6 to 3 seconds and do not depend on the frame rate; a
  bot's random choices replay from its seed.
- `server_test_clients` (`scripts/tests/test_server_test_clients.py`): the
  extracted `SV_UpdateBots` asks the brain about active bots only, and a brain
  that takes the command replaces the random inputs.

Not covered by these checks: a bot in a real match. That needs the game's
assets, so it runs on a machine that has them.
