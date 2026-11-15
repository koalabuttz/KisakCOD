# KisakCOD bots

KisakCOD is getting its own multiplayer bots: opponents that play fair and
play like people. The research and the full plan are in the project's shared
folder (`bots/sota-bots-research.md`); this guide says what is in the code
now and how to use it.

## What "fair" means here

- A bot plays through the same door as a player. Its only output is the
  `usercmd_s` a player's client sends (view angles, movement, buttons), which
  the server runs through `SV_ClientThink` and Pmove. It cannot turn faster,
  move differently or shoot differently from a player: spread, sway,
  aim-down-sight time and damage are the game's own. Two things a player's
  client adds to every command, the weapon's view kick and the view error on
  entering the sight, the bot adds to its own (see "How a bot fights").
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

With `bot_brain 1`, `bot_combat` says whether they fight:

| `bot_combat` | KisakCOD's bots |
|---|---|
| `0` | never fire: they walk, look and notice |
| `1` (default) | fight the opponents they see |

`bot_log 1` prints each player a bot notices, the first shot of each fight,
each choice to take cover or hunt, and its team play (parts, callouts, where
its part sends it).

`bot_brain`, `bot_combat` and `bot_log` are settings this port added, so they
are on the list a game server may not set on a player's client
(`src/qcommon/server_dvar_guard.h`).

## What the bots do now (phase 4c)

The brain takes over the test clients' commands. Each frame a bot decides what
to do (see "How a bot decides"), walks there on the map's navigation mesh with
the movement keys a player would press, turning at up to 200 degrees a second,
jumps when it has made no headway for 1.2 seconds (once a second, twice at most
until it gets closer) and finds another way after 2.5. Without a mesh it stands and looks around at a human pace (a new
direction every 1.5 to 4 seconds, up to 120 degrees a second). It carries on
from wherever the server puts its view (a spawn, a killcam, a script's
`setPlayerAngles`) and taps use while dead.

A bot that sees an opponent it noticed fights them (with `bot_combat 1`, see
"How a bot fights"); with `bot_combat 0` it watches them while it walks on.
It keeps its aim where it last saw one it lost, and looks toward a sound or a
compass dot it heard, knifes a player within reach and throws a frag where it
just lost one (see "How a bot fights"). It reloads an empty clip, or one under half when it has
not fought for two seconds, and switches to its other gun when one runs dry.

On a team, bots call out the enemies they see and play the part their
team's commander gives them (see "How a team of bots plays").

Next: peeking from cover, smoke and flash grenades, killstreaks, class choice,
jumps, mantles and ladders, stance in fights, then difficulty levels and
per-bot style, start-a-game and start-a-server options, and the PS Vita's bot
budget (6v6 TDM first, then 9v9 Ground War).

## How a bot decides

`src/bots/bot_mind.h` scores four intents each frame from what the bot knows,
the way shipped shooter bots weigh their options:

| Intent | When | What it does |
|---|---|---|
| fight | a noticed enemy is in view | aims and fires (below); keeps walking its way or strafes |
| cover | in a fight (or just hit) and reloading, badly hurt or facing several enemies, and it knows cover | walks to a place hidden from the enemy, waits there until reloaded and healed (the stock health comes back after a few seconds out of fire), then goes back in |
| hunt | it lost sight of an enemy, heard one, or was hit from a direction | walks to where it last saw them, the sound, or the way the hit came from; drops the lead when it gets there and finds nobody |
| roam | otherwise | walks to a place it has not seen for a while, more often one where it met enemies lately |

- **Personality.** Each bot draws an aggression and a patience when it joins.
  A bold bot hunts more and takes cover later; a patient, calm one more often
  stops when it gets somewhere and holds an angle for 1 to 10 seconds,
  crouched or standing.
- **No flip-flopping.** The current intent gets a bonus, and a hunt or cover is
  kept for at least 1.5 s unless a fight starts.
- **Its own memory.** Where a bot has been and met enemies is its own grid of
  256-unit cells: what it saw, heard or was hit from, and what its team called
  out.
- **The hit indicator.** A player's HUD shows the direction a hit came from (the
  game puts it in the player state); a bot turns that into a lead to hunt or a
  threat to hide from.
- **Pre-aiming.** While it walks with nobody in view, a bot looks at the last
  point of its way it can still see, at head height: where the way turns out
  of sight round a corner, which is where an enemy would appear. It checks
  every 64 units up to 960 along the way, four times a second, with sight
  traces through the map's solid geometry only.
- **Cover.** Once the map's spots are worked out (below), `src/bots/bot_tactics.cpp`
  takes the nearest spot within 450 units that the spot nearest the threat
  does not see. Before that, or when none fits, it tries 12 random places on
  the mesh within about 450 units. Either way it keeps the nearest place, at
  most a 700 unit walk, where a standing player's chest and head are hidden
  from where the bot thinks the enemy's eyes are, checked with real traces.
  It looks at most once a second and keeps a spot while it still hides it.
  A bot that is reloading in a fight takes cover unless it is bold.

The decisions read only what the bot knows (its perception, its own player
state, the map's geometry): cover is from where it thinks the enemy is, and a
hunt goes to where it last saw or heard them.

## How a team of bots plays

`src/bots/bot_team.h` is what a team says and who does what:

- **Callouts.** A bot that sees an enemy calls out where, at most every 3
  seconds per enemy. Its team hears it 0.6 to 1.2 s later (the moment it takes
  to say), at a place rounded to a 128-unit grid (a callout names an area, not
  a point), and acts on it for 15 s. Teammates put callouts into their memory
  of where enemies were. Every bot is its own team in free-for-all.
- **The commander.** Once a second it gives each bot of a team a part, from
  the team's size, the objectives on its compass and the bots' personalities:

  | Part | Who | What it does when nothing more pressing is on |
  |---|---|---|
  | push | the boldest | goes to the latest callout (10 s at most), then roams |
  | anchor | the most patient | goes to a spot that overlooks the callout nearest it, holds there for 7 to 35 s, crouched or standing, watching it |
  | attack | the boldest, when the compass shows places to take | goes to the objective and stays on it, holding use (a flag captures while someone stands in it, a bomb plants or defuses while use is held), lying prone (more often the more patient it is), crouched or standing; on its way and on it, it fights an enemy in view but does not leave to chase a sound or a lead, and a prone bot stays down on the flag and fights from there |
  | defend | the most patient, when the compass shows places to hold | holds a spot that overlooks the objective for 8 to 35 s, watching it |
  | roam | the rest | roams as before |

  With no objectives (team deathmatch), about 40% push, 25% anchor and the
  rest roam. Attackers and defenders are spread over the objectives
  nearest the team first, so a team takes the flags on its side of the map
  before it contests the others. Fights, cover and hunts come first: a part is what a bot does
  between them.
- **Objectives.** A bot reads them off its own compass: the objectives the game
  sends its team, active ones only. The stock gametypes mark them with
  `compass_waypoint_*` icons: "defend" ones are to hold, the rest (capture,
  target, bomb, defuse) to take. An icon on an entity (a carried flag or bomb)
  follows it, as on the compass. Other icons (killstreak markers) are not
  objectives. `bot_log 1` prints a team's objective count when
  it changes.

`src/bots/bot_spots.h` is the map knowledge a player gets from playing a map a
few times, worked out once per map while the bots play: spots 200 units apart
over the mesh (at most 512, from 4000 random places, 100 a server frame), then
for every pair whether a standing player at one sees the head of one at the
other, 200 lines a server frame. On mp_crash-sized maps that takes about half
a minute. Bots use it for cover (above) and to find where to anchor and defend
from: a spot that sees the place and that few other spots see. A spot stands
for the area around it, so a bot checks with a real trace before it relies on
one.

## How a bot fights

`src/bots/bot_combat.h` is the bot's hand and trigger. It fights only the
player its perception noticed and sees now, and aims at where it thinks that
player is, never at where they really are:

- **Reaction.** After noticing (at least 0.2 s in view), the hand starts after
  a further 120 ms on average (deviation 40, never under 60): a player's
  decision and motor onset.
- **Flick, then settle.** The hand makes aimed movements at up to 450 degrees a
  second. Each lands off by a normal error of 0.35 degrees plus 7% of the
  movement's size, so a 60 degree flick lands a few degrees off and a small
  correction lands close; a new correction starts every 170 ms or so. In the
  host test the first moment on a still player 5 to 64 degrees off comes a
  median 0.45 s after the notice, and a second after a 60 degree flick the
  error is under 0.6 degrees in nine runs out of ten.
- **Tracking.** The bot sees a moving player ten times a second (its sense
  ticks) and leads 60% of the motion it saw since, for at most 0.15 s. It
  trails a player strafing back and forth: in the host test it is on one
  strafing at 500 units about a fifth of the time, against all the time on a
  still one.
- **Trigger.** It fires while its view is within the player's angular
  half-width of where it thinks they are (never tighter than half a degree).
  It clicks a semi-automatic about six times a second (never more than every
  110 ms), holds an automatic close up and fires it in bursts of about 0.45 s
  beyond 900 units. It aims down the sight beyond 400 units or with a scope,
  and fires then only once the sight is up.
- **Recoil.** On a player's client, `CG_KickAngles` adds the weapon's view kick
  to every command (`CL_CreateCmd` sends `viewangles + cgameKickAngles`); the
  server only moves the gun model. A bot has no client, so it would have no
  kick at all. The brain therefore runs the same kick for its own shots, with
  the weapon's numbers and the same 5 ms steps, and adds it to its command;
  the hand pulls back half of it, 0.15 s late. Entering the sight throws its
  view off by the weapon's ADS view error, as `CG_PredictPlayerState` does a
  player's, and the hand has to find the player again.
- **Moving.** While it fights it keeps walking its way or strafes for 0.4 to
  1.2 s at a time, more often the other way than the last.
- **The knife.** Within the game's melee range (`player_meleeRange`, plus 8
  units) and with its view within 25 degrees of the player, after the hand's
  onset, it knifes instead of firing, at most every 0.45 to 0.7 s.
- **Grenades.** Once for each enemy it loses sight of, 0.4 to 3.5 s after, a
  bot may throw a frag where it last saw them, 250 to 1000 units away. The
  chance is 20% plus half its aggression. `src/bots/bot_grenades.h` finds the
  flattest view pitch whose arc comes down at the enemy's feet. The game
  throws along the view at the weapon's projectile speed, plus its upward
  and forward speeds, with gravity pulling the grenade down. The bot throws
  only when the arc clears the map until 96 units from the target. It lines
  up the view at its usual turning pace (giving up after 1.5 s or when an
  enemy comes into view), holds the frag button 0.25 to 0.6 s while the fuse
  runs, and lets go. It waits 10 to 20 s before the next throw.

The numbers are one decent player's; difficulty levels (phase 5) change them.

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
- **Compass.** An opponent the HUD shows as a red dot gives a place as the
  compass shows it (off by up to 5% of the distance): one a script pings, and,
  while the bot's side has a UAV (its own player state's `radarEnabled`, which
  is what the HUD reads), every opponent without UAV Jammer
  (`specialty_gpsjammer`) at each radar sweep, every 4 s
  (`compassRadarUpdateTime`).
- **Smoke.** A smoke cloud is only the client's effect, so the server never
  knew where it was. The bots keep their own estimate from each smoke
  grenade's burst (the event entity `G_ExplodeMissile` spawns): a ball 64
  units above the burst that grows to 320 units across in 2 s, holds, and
  shrinks to nothing over the last 5 of 45 s. It hides any of head, chest and
  hips the line from the eye passes through, as a wall would. The size is an
  estimate, kept on the large side so a bot never sees through smoke a player
  could not.

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
| `src/bots/bot_mind.h` | what a bot decides: intents and their scores, personality, the memory of places, the pre-aim point; no engine types |
| `src/bots/bot_team.h` | a team's callouts, the commander's roles, objectives from compass icons; no engine types |
| `src/bots/bot_spots.h` | a map's spots and which see each other, cover and overwatch queries; no engine types |
| `src/bots/bot_tactics.cpp` | the map sense on the server: clear lines through the map, the spot table worked out a frame at a time, cover from a threat, overwatch |
| `src/bots/bot_combat.h` | the hand and trigger: reaction onset, aimed movements and corrections, tracking lead, firing, the view kick, strafing; no engine types |
| `src/bots/bot_grenades.h` | a grenade's arc and the view pitch that lands it on a target; no engine types |
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
  footsteps and shots are heard in range and only roughly; a smoke cloud
  grows, holds and fades, and hides what is behind or inside it.
- `bot_combat` (`scripts/tests/test_bot_combat.py`, in `run_linux_checks.py`):
  builds `tests/bots/bot_combat_test.cpp` on the host, 64-bit with sanitizers
  and 32-bit. Over 1000 runs the hand never moves before its onset or faster
  than its top speed and is never on a player sooner than 100 ms after the
  notice; a big flick takes longer and lands further off than a small one,
  and corrections close in; a strafing player is harder to stay on than a
  still one; a semi-automatic is clicked no faster than every 110 ms and
  released between clicks, an automatic held close up and fired in bursts at
  range; a shot kicks the view up and it comes back, never past 10 degrees,
  and the hand pulls back only part of it, late.
- `bot_grenades` (`scripts/tests/test_bot_grenades.py`, in
  `run_linux_checks.py`): builds `tests/bots/bot_grenades_test.cpp` on the host,
  64-bit with sanitizers and 32-bit. A throw comes down where it aims, and it
  is the flattest throw that gets there. A further target needs a higher
  pitch and one below the eye a lower one. A target past the weapon's reach
  gets no throw. A forward speed flattens the throw. The arc ends at the
  target, in the direction the bot faces.
- `bot_mind` (`scripts/tests/test_bot_mind.py`, in `run_linux_checks.py`):
  builds `tests/bots/bot_mind_test.cpp` on the host, 64-bit with sanitizers
  and 32-bit. A bot roams with nothing known, fights what it sees, takes the
  cover it knows to reload, to heal or against three enemies (a calm bot; a
  bold one wants it less), fights on with no cover or nothing to reload,
  leaves cover once healed, hunts a lost enemy or a sound and drops a
  forgotten lead; small swings in a score do not change its mind, and an
  enemy in view ends a hunt at once. It roams to places it has not seen and
  where enemies were lately (more so when bold), and pre-aims where its way
  turns out of sight round a corner, at head height.
- `bot_team` (`scripts/tests/test_bot_team.py`, in `run_linux_checks.py`):
  builds `tests/bots/bot_team_test.cpp` on the host, 64-bit with sanitizers
  and 32-bit. Objectives are read off compass icons; a callout is not heard
  before it is said, names an area, is dropped when old, and the same enemy
  close by updates it; the commander's roles fit the team's size and
  objectives, the boldest push or attack and the most patient anchor or
  defend, and the same team keeps its roles. In a made-up map with a wall and
  a door the spot table is filled a step at a time within its budget, matches
  every line, finds cover the threat does not see (nearest first) and places
  that see a target (least seen first).
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
  yard builds in tiles; points picked near a bot stay near it. It also
  builds the engine's brush-to-faces code (`CM_ShowSingleBrushCollision`, until now only a debug drawing) with
  AddressSanitizer and checks a box and a wedge give exactly their faces. PC
  bot brain run 3 crashed there: its corner buffer held one point where up to
  1024 are written, and a corner on an axial plane was compared with the
  previous brush's sides.
- `bot_driver` (`scripts/tests/test_linux_bot_driver.py`): the live probe's
  view and place evidence cannot pass with missing samples, a bot that never
  turns or never walks, a snap faster than the motor, a pitch past the limit or
  a bot faster than a player; a death and spawn is no turn and no walk; a
  first shot at a teammate, or sooner than the human floor, fails.

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
200 ms; it prints how many there were. The probe sets `bot_combat 0` so every
bot stays alive. With `--combat` the bots fight: only the first hold sample
must be healthy, a quarter second to or from one a bot was not playing is no
turn and a second of it no walk, turns may reach the aiming hand's 450 degrees
a second (with the kick on top), and each fight's first shot must be at an
opponent, at least 60 ms after noticing and 260 ms after the player came into
view; on both teams at least one bot must have fired. It prints how often the
bots opened fire and the hits and kills the game logged. With `--brain 1` it
also prints how often bots took cover and went hunting, the parts the
commander gave, the callouts, pushes, anchors and objective moves, and when
the map's spots were ready, and with `--combat` how many grenades bots threw
and how often they knifed. With `--objectives` (and more than four bots) the
fixture starts the stock Domination game type instead of team deathmatch: the
bots of both teams must read flags off their compass (`bots: team N
objectives: take T, hold H`), and at least one must go to take one. Views are a
quarter second apart because at one a second any turn would fit: the short way
round is never more than 180 degrees.

A stock team holds at most half the server's slots, so the probe puts more than
four bots on both teams (odd entity numbers on axis) and keeps the retail
random inputs from firing.
It takes up to 18 bots, 9 against 9; `sv_maxclients` is the larger of 8 and the
bot count. PC bot brain run 1 sent 6 and 8 bots all to allies before this, and
every bot past the fourth stayed a spectator.
