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
- A bot only knows what a player could know: what it saw on its screen, what
  it heard, and what its own HUD shows (see "What a bot knows" below). It does
  not read other players' positions from the server to decide anything.

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

## What the bots do now (phase 2)

The brain takes over the test clients' commands. A bot that is playing walks to
random places on the map's navigation mesh: it turns toward the next corner of
its way at up to 200 degrees a second, walks there with the movement keys a
player would press, looks around for half a second to two and a half when it
arrives, jumps when it has made no headway for 1.2 seconds and picks another
place after 2.5. Without a mesh it stands and looks around at a human pace (a
new direction every 1.5 to 4 seconds, up to 120 degrees a second). It carries
on from wherever the server puts its view (a spawn, a killcam, a script's
`setPlayerAngles`) and taps use while dead.

A bot that noticed an opponent watches them while it walks on, turning at up
to 200 degrees a second; it looks where it last saw one it lost, and toward a
sound or a compass dot it heard. It does not aim or shoot yet.

Next phases, in order: smoke, the UAV and objectives for perception, the motor
model and combat for TDM and FFA, decisions and objective modes, then
difficulty levels, start-a-game and start-a-server options, and the PS Vita's
bot budget (6v6 TDM first, then 9v9 Ground War).

## What a bot knows

Ten times a second each bot senses the other players (`src/bots/bot_senses.cpp`)
and `src/bots/bot_perception.h` turns that into what it knows. Teammates are
left out; in a free-for-all everybody is an opponent.

- **Sight.** An opponent counts only when they are on the bot's screen (within
  40 degrees across and 24 up or down of where it looks, about what `cg_fov 65`
  shows on a 16:9 screen), in its potentially visible set and inside the map's
  fog (`SV_inSnapshot`, the test the server uses before it sends a player to a
  client), and then only as much of them as a clear line from the bot's eye
  reaches: head, chest and hips, through what blocks a player's sight
  (`MASK_AIMTARGET_VISIBILITY`: walls, foliage, no-sight brushes, vehicles).
- **Noticing.** What the bot sees fills a meter. A running opponent close by in
  the middle of the screen fills it in 0.3 s. It fills more slowly with
  distance (beyond 600 units, in proportion), toward the screen's edge (to 30%
  of the rate), for a still player (half), a crouched or prone one (three
  quarters, half) and for one mostly behind cover. Nobody is noticed in less
  than 0.2 s in view, however things fall: that is the floor of a human's
  reaction. A meter that is not full drains when the opponent goes out of
  sight.
- **Memory.** A noticed opponent who goes out of sight stays where the bot
  last saw them; the bot's confidence halves every 3 s and it forgets them
  after 10 s.
- **Hearing.** Footsteps carry 250 units walking, 500 running and 800
  sprinting; crouched, prone and standing-still players and those with Dead
  Silence (`specialty_quieter`) make none. A shot carries 4000 units, 300
  silenced. A sound gives a place off by up to 15% of its distance (half that
  for a shot) and turns the bot's head for 2 s; it does not count as noticing.
- **Compass.** An opponent the HUD shows as a red dot (pinged by a script) gives
  a place as the compass shows it.

`bot_log 1` prints each opponent a bot notices, for example
`bots: 3 noticed 4 after 300 ms in view, 912 units, 12 degrees off centre, 67%
showing`. Like `bot_brain`, it is on the list a game server may not set on a
player's client.

## The navigation mesh

The mesh says where a player can walk and finds the way from one place to
another. It is built with Recast and queried with Detour
(`deps/recastnavigation`, version 1.6.0, zlib licence) from the map's
player-solid collision: every brush's faces and the terrain and patch
triangles. The agent is a standing player: 15 units across from the middle, 70
tall, steps up 18 units and walks slopes up to 45 degrees. Cells are 6 units
across and 3 high, in tiles of 64 cells.

The mesh is built the first time a bot needs it on a map, on the server's own
frame, and saved as `bots/nav/<map>.kbnav` in the home folder with the map's
collision checksum; later loads read it back when the checksum matches. The
server prints which it did:

```
bots: navigation mesh for mp_crash built in <seconds> s (geometry ...)
bots: navigation mesh for mp_crash loaded from bots/nav/mp_crash.kbnav (...)
```

The cache is read only from the home and base folders, never from a game
archive or a server's download, and each tile's size is checked before Detour
sees it. The mesh is derived from the map's data, so it is never committed.

Not in the mesh yet: static models' collision (props), entities (doors,
vehicles, script models), and jumps or mantles between floors. A bot that
walks into one notices it is stuck, jumps, then takes another way.

## Code

| Where | What |
|---|---|
| `src/bots/bot_brain.h`, `bot_brain.cpp` | `bot_brain`, and `Bot_UserMove`: one bot's command for this server frame |
| `src/bots/bot_perception.h` | what a bot knows: the noticing meter, memory, hearing, what to look at; no engine types |
| `src/bots/bot_senses.cpp` | `Bot_Sense`: one sense tick of a bot's eyes, ears and compass on the server |
| `src/bots/bot_motor.h` | the view motor, command angle conversion, the movement keys toward a point and each bot's random source; no engine types |
| `src/bots/bot_navmesh.h`, `bot_navmesh.cpp` | building the mesh from a triangle soup, the cache blob, and the queries (nearest point, path, random reachable place); no engine types |
| `src/bots/bot_nav_world.cpp` | `Bot_Nav`: the current map's collision as triangles, and the cache in the home folder |
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
- `bot_perception` (`scripts/tests/test_bot_perception.py`, in
  `run_linux_checks.py`): builds `tests/bots/bot_perception_test.cpp` on the
  host, 64-bit with sanitizers and 32-bit. Nobody off the screen, behind cover
  or past the sight range is noticed; over 2000 random runs nobody is noticed
  in under 0.2 s in view; distance, the screen's edge, stillness, crouching
  and cover slow noticing; a lost player is remembered, then forgotten;
  footsteps and shots are heard in range and only roughly.
- `server_test_clients` (`scripts/tests/test_server_test_clients.py`): the
  extracted `SV_UpdateBots` asks the brain about active bots only, and a brain
  that takes the command replaces the random inputs.

- `bot_navmesh` (`scripts/tests/test_bot_navmesh.py`, in
  `run_linux_checks.py`): builds `tests/bots/bot_navmesh_test.cpp` with Recast
  and Detour on the host, 64-bit with sanitizers and 32-bit. In made-up rooms a
  bot finds the way through a 64-unit door but not a 24-unit gap, up a 16-unit
  step but not a 40-unit ledge, up a 25 degree ramp but not a 60 degree one;
  it never walks on a floor's underside or between islands; a cache blob with
  a wrong checksum, a cut or a bad tile size is refused; and an 8000 by 8000
  yard builds in tiles. It also builds the engine's brush-to-faces code
  (`CM_ShowSingleBrushCollision`, until now only a debug drawing) with
  AddressSanitizer and checks a box and a wedge give exactly their faces. PC
  bot brain run 3 crashed there: its corner buffer held one point where up to
  1024 are written, and a corner on an axial plane was compared with the
  previous brush's sides.
- `bot_driver` (`scripts/tests/test_linux_bot_driver.py`): the live probe's
  view and place evidence cannot pass with missing samples, a bot that never
  turns or never walks, a snap faster than the motor, a pitch past the limit or
  a bot faster than a player.

Live, with the game's assets (a machine that has them):

```
python3 scripts/tests/test_linux_bots.py <cod4 dir> --binary bin/KisakCOD-dedi \
    --bots 6 --hold-seconds 20 --brain 1
```

holds six bots on mp_crash with `bot_brain 1` and, besides the usual admission,
health, team, class and clean-quit checks, requires the server's navigation
mesh line, logs every bot's view four times a second and its place once a
second, and requires that each bot looked around, never turned faster than 200
degrees a second (with a margin), kept its pitch within the player's limits and
walked at least 300 units, never more than 300 in a second. With `bot_log 1`
it audits every notice: an opponent (with up to four bots, all on one team,
there are none), on the bot's screen, partly in view and in view for at least
200 ms; it prints how many there were. Views are a
quarter second apart because at one a second any turn would fit: the short way
round is never more than 180 degrees.

A stock team holds at most half the server's slots, so the probe puts more than
four bots on both teams (odd entity numbers on axis) and keeps them from firing.
It takes up to 18 bots, 9 against 9; `sv_maxclients` is the larger of 8 and the
bot count. PC bot brain run 1 sent 6 and 8 bots all to allies before this, and
every bot past the fourth stayed a spectator.
