# Singleplayer levels on Linux: sweep tool and run plan

Status: killhouse and cargoship play on a developer laptop
([linux-sp-game.md](linux-sp-game.md)). Sweep run 1 (below) loaded and ran
all 21 levels unattended, changed missions in one process, and saved and
loaded. No level beyond the first two has been played by hand yet. This page
lists what the other 19 levels use that those two do not, gives a tool that
runs many levels in one go, the order to run them in, and the results.

## What later levels use that the first two do not

The engine pieces below are reached only by some levels, or only after a
level ends. The level notes come from the game's public mission design, not
from its files.

| Piece | Where in the code | Reached by |
| --- | --- | --- |
| Loading a second level in the same process | `SV_SpawnServer` after `CL_ShutdownAll`, zone unload and reload, `R_` world reload through DXVK | every mission change (`missionsuccess` -> victory screen -> `spmap`) |
| Saves written to disk and loaded back | `savememory.cpp`, `g_save.cpp`, `savedevice_pc.cpp` (`save/temp.svg` renamed into `players/profiles/<name>/save/`), `SV_LoadGame_f`, `SV_MapRestart(1, 0)` | autosaves, and every death's "retry" (`loadgame_continue`) |
| End-of-level movie | `G_CheckReloadStatus` sends `cinematic <name> 0`; `CIN_PlayCinematic` takes the client to `CA_CINEMATIC`; `SCR_StopCinematic` runs the `nextmap` dvar | levels that end on a movie rather than the victory screen |
| In-game movies | `cinematicingame*` script calls -> `R_Cinematic_SetNextPlayback` (looping and memory-resident flags) | screens inside a level |
| Streamed sound | OpenAL streaming in `snd_driver_openal.cpp` (dr_wav/dr_mp3 over `FS_Read`/`FS_Seek`) | dialogue and music in every level; the first runs were silent |
| Night vision | `visionsetnight`, `precachenightvisioncodeassets`, night-vision weapon state in `cg_weapons.cpp` | blackout, bog_a, icbm and other night levels |
| Time scale | `settimescale` script call -> `com_timescale`; the server thread's timeout scales with it (`Sys_SetServerTimeout`) | slow-motion moments (airplane, jeepride ending) |
| Vehicles and helicopters | `g_scr_vehicle.cpp`, `g_vehicle_path.cpp`, `g_helicopter.cpp`, player linked to a moving entity | coup (car ride), bog_b (tank escort), armada, airlift, jeepride |
| Spot light shadows | the renderer's spot shadow path (needs moving time) | helicopter searchlight in hunted |
| Gunship view | full-screen overlays, several weapon FOVs, long view distance | ac130 |
| Guided launcher HUD | Javelin overlay and top-attack missile in `cg_weapons.cpp`/`g_missile.cpp` | launchfacility_a |
| Many AI at once, dogs | `actor_*.cpp`, `actor_dog_exposed.cpp` | village_defend, scoutsniper, sniperescape |

### Linux differs from the Windows baseline in one known way

The Linux game keeps assertions on in every configuration (`RELEASE_ASSERTS`
in `sp_game.cmake`). The Windows `KisakCOD-sp` Release build, which the
Windows baseline used, compiles `iassert` out. So a Linux run can stop at an
assertion that the Windows Release build passes without a word. Before
calling such an assertion a Linux bug, check it against a Windows Debug build
or read the code: killhouse's pregame assertion was a bug both builds shared.

## Fixes made while reading

- **Streamed sound seeks.** `FS_Seek` takes the engine's origins (0 from here,
  1 from the end, 2 from the start) for every file, loose or packed.
  The OpenAL stream callbacks converted dr_wav/dr_mp3's C-library origins
  only for files packed in an `.iwd`, so a loose sound file seeked from the
  wrong place. The stock sounds are packed, so this mattered only for loose
  files, such as a mod's. The driver is shared with `KisakCOD-mp`.
  `scripts/tests/test_openal_stream_seek.py` checks the conversion.
- **Survey mode for assertions.** `KISAK_ASSERT_CONTINUE=1` (environment)
  makes `MyAssertHandler` report each failing assertion site (its first
  failure and every power of two after it) and return, as the Windows assert
  dialog's Ignore button does. Off unless set. It is for listing more of what
  a level trips over in one run. A level that runs only in this mode has not
  passed. `scripts/tests/test_linux_assert_continue.py` checks both modes.

## The sweep tool

`scripts/tests/run_linux_sp_level_sweep.py` runs `KisakCOD-sp` against an
install and writes a report. It is not run by CI.

```sh
python3 scripts/tests/run_linux_sp_level_sweep.py "$GAME" --binary "$BUILD/KisakCOD-sp" \
    --levels killhouse,cargoship,coup --hold 60 --work-dir ~/kisak-private/sp-sweep1
```

- `--plan levels` (default) starts one process per level with `+devmap`, waits
  for `Load time: N msec`, holds, asks for `mapname` on the console (proof
  the main loop still answers), and quits.
- `--plan chain` loads the first level, then each next one with `spdevmap` in
  the same process, the way a mission change does.
- `--save-load` adds `devsave sweep`, a check that
  `home/players/profiles/sweep/save/sweep.svg` appeared, `loadgame sweep` and a
  second hold.
- `--screenshot` adds `screenshot sweep_<level>` and reads the image the game
  wrote under `home/main/screenshots/`. The step "frame drawn" is seen when
  the file is there and is not nearly all one colour, and the report gets a
  table of numbers for each frame: size, mean brightness, share that is not
  black, colour count, and the mean colour of the top, middle and bottom
  third. A black or one-colour screen shows up with nobody watching, and a
  level whose row stands apart from the others is worth a look. The images
  are game output: they stay on the machine (`--delete-screenshots` removes
  each one once it is measured).
- `--death-retry` adds, last in each level, `kill`, a wait for the retry's
  load (the game starts `loadgame_continue` by itself 3 seconds after the
  death), a hold and the `mapname` question again. The retry loads the last
  save: the level's own start save or autosave, or `--save-load`'s when both
  are asked for. A level with no save at all stops with "Save '' not
  available", which the report lists.
- `--assert-continue` sets `KISAK_ASSERT_CONTINUE=1`. `--gdb` runs the game
  under `gdb -batch` and keeps the backtraces in the log.
- `--set DVAR VALUE` adds a `+set`. Default window 1280x720, `snd_volume 0.8`.

Each run gets a work directory: `base/` holds symlinks to the install's
`main`, `zone` and `localization.txt`. `home/` is `fs_homepath`, seeded with a
profile named `sweep`, since `+devmap` without a profile stops at "Tried to use
a player profile before it was set". `xdg/` is `XDG_DATA_HOME`. The game
writes only under the work directory. Each level's console output is in
`<level>.log`, and the summary in `report.md`.

The report says which steps were seen, the load time, how the process
ended, and the assertions, errors and warning count. It does not say that a
level looks or plays right: the frame numbers only rule out a blank screen.
Looking at the screen and hand play do the rest.

## Laptop run plan

Build: the laptop's `sp-rel-al` (RelWithDebInfo, OpenAL, cinematics on),
rebuilt at the commit under test. Sound on: the sweep sets `snd_volume 0.8`.

1. **Sweep, assertions fatal, each level on its own.** All 21 levels with
   `--hold 45`. This is the list of levels that load and run, and the first
   failure of each one that doesn't.
2. **Same sweep with `--assert-continue`** for the levels that stopped at an
   assertion in step 1, to see what else each one hits. Report the extra
   assertions; don't count those levels as passing.
3. **Fix pass** in the cloud from the logs. Rerun the fixed levels with
   `--gdb` when a crash needs a stack.
4. **Mission changes:** `--plan chain --levels killhouse,cargoship,coup,blackout --hold 30`.
   This frees a level and loads the next in one process, which no Linux run
   has done yet (multiplayer included).
5. **Saves:** `--save-load --levels killhouse,cargoship,blackout,ac130`. This
   is the path a death's retry takes.
6. **Hand play** by David on the levels that pass: night vision in blackout,
   the gunship in ac130, the Javelin in launchfacility_a, the slow-motion
   breach in airplane, one death and retry, and one real mission end through
   the victory screen (the sweep cannot click it).
7. **Frames and deaths:** all 21 levels with `--hold 30 --screenshot
   --death-retry`. This says whether each level draws something and whether a
   death brings the level back, with nobody at the laptop, so it can run
   before step 6. Run on i386 (sweep run 2 below): every level passes.

Report back the `report.md` tables and the failing levels' logs. Report
console lines and file names, never game file contents.

## Sweep run 1 (laptop, 2026-11-07)

Commit 0ce99b9 (master with PR 98), i386 `sp-rel-al`, sound off, 1280x720.

- **Step 1, all 21 levels:** each loaded in 4 to 21 s (killhouse, the first,
  21 s; most 7 to 10 s), held 45 s and quit with exit 0. No assertions, no
  errors, no signals. Steps 2 and 3 had nothing to run.
- **Step 4, chain** killhouse, cargoship, coup, blackout: all four loaded in one
  process (later loads 5 to 6 s), exit 0.
- **Step 5, save and load** on killhouse, cargoship, blackout and ac130: the save
  file was written, `loadgame` reloaded each level, the console answered after
  it, exit 0.
- The sweep's own failures came from two tool bugs, both fixed: the console
  probe asked `sv_mapname` (never set in singleplayer; the server's dvar is
  `mapname`), and the save check took the "time to write" line of the
  level-start save being flushed again for the `devsave` one.
- Engine messages (the Windows x64 game prints the same ones, see the tour
  below): hunted prints "Max scene drawsurfs
  exceed" 5 times, ac130 "Too many visible static models" twice, airplane "AI
  couldn't find path to goal" twice. Six levels print the "computer changed"
  profile warning at start: the CPU estimate (`sys_configureGHz`) varies
  between processes by more than the profile's 10 % check. The profile is left
  unchanged.
- Nobody watched the screen. Hand play (step 6) is next.

## x86_64 sweep run 1 (laptop, 2026-11-10)

The first runs of the x86_64 game with game data, RelWithDebInfo, sound off,
1280x720, the same laptop. Local runs, not CI evidence.

- **Built without movies** (`KISAK_LINUX_CINEMATICS` off, commit b1a8f06):
  killhouse and cargoship both ended with signal 6 while the loading screen
  was up (`!r_glob.screenUpdateNotify`, `rb_backend.cpp`). The error under it
  was "Tried to use 'cinematicY' when it isn't valid": the loading screen
  samples the movie's images and that build never set them. Fixed
  ([linux-sp-game.md](linux-sp-game.md#fixes-the-first-level-loads-found)).
  With the fix (commit a418999), killhouse and cargoship load and pass every
  step in a build without movies (PC, Linux in WSL, below). The i386 runs
  never met this because they always had movies on.
- **Built with movies** (the same commit): killhouse and cargoship load in 13
  and 11 s and pass every step.
- **Step 1, all 21 levels** (commit 3019927, movies on): every level loads,
  holds 45 s, answers the console and quits with exit 0. airlift is among
  them. Without that commit's fix the Windows x64 game crashes there
  (below); the x86_64 Linux game does not (PC, Linux in WSL, below).
- **Step 4, chain** killhouse, cargoship, coup, blackout in one process, and
  **step 5, save and load** on killhouse, cargoship, blackout and ac130
  (commit 3019927, movies on): all steps seen, exit 0.

## Windows x64 tour (PC, 2026-11-10)

The hosted x64 Release build of commit 65bd0e3 on the Windows PC, as a
comparison on the same game data. A local run, not CI evidence.

- 20 of 21 levels load, run 45 s and quit with exit code 0. **airlift crashed
  4.5 s into the level, three launches of three** (and with the x64 build of
  7 November): a read of address 0xc in `Phys_ObjCreate`, called from
  `FX_SpawnModelPhysics`. The cause is a 64-bit-only wrong read, fixed in
  shared code. With the fix (the hosted x64 Release build of commit
  a418999), airlift held 60 s in the level and quit with exit code 0 three
  launches of three, and killhouse 45 s.
- The level chain (killhouse, cargoship, coup, blackout in one process), a
  save loaded in the same process and in a fresh one, and a death with its
  retry all work there.
- The engine messages of sweep run 1 are not port bugs: Windows prints them
  too. hunted "Max scene drawsurfs exceed" 5 times, ac130 "Too many visible
  static models" twice, "couldn't find path to goal" in airplane twice and in
  armada 19 times.

## x86_64 fix checks (PC, Linux in WSL, 2026-11-10)

Three runs of the level sweep on the Windows PC's Linux (WSL, Xvfb, Mesa's
software Vulkan device, sound off), each level held 30 to 60 s. Local runs,
not CI evidence.

| Build | Levels | Result |
| --- | --- | --- |
| master 85dad4f7, before both fixes | killhouse, airlift | all steps seen, exit 0 |
| a418999, both fixes | killhouse, airlift | all steps seen, exit 0 |
| a418999, built without movies | killhouse, cargoship | all steps seen, exit 0 |

No assertion, error or warning in any of the six logs. The unfixed build
holding airlift means the wrong read lands on harmless bytes in this Linux
build; the Windows runs above are what show the fix. Under software
rendering a level took 25 to 54 s to load, against 5 to 9 s with a GPU on
Windows; the sweep's default load timeout (240 s) covers that.

## Sweep run 2, frames and deaths (laptop, i386, 2026-11-10)

Commit 3019927, i386 `sp-rel-al`, movies on, sound off, step 7 above
(`--hold 30 --screenshot --death-retry`). The first run of both options on
the game. A local run, not CI evidence.

- **All 21 levels** pass every step: loaded, held 30 s, frame drawn, console
  answers, `kill`, the game loads its last save by itself, held 15 s,
  console answers again, exit 0. No assertion, no error.
- **Frames** (mean brightness 0 to 255): 29 (hunted) to 143 (ambush) for 20
  levels. launchfacility_b was 226, nearly white. That is the level's own
  picture: its opening film plays from the start of the load for about
  52 s, then the game shows plain white for about a second and a half and
  fades into the level. The Windows x64 game does the same (a series of
  frames on the PC). The laptop's load took 22.7 s, so 30 s after it fell in
  that white. A frame taken a fixed time after the load can land in a
  level's opening film on any level that has one.
- In a two-level warm-up before the full run, cargoship's frame was black
  (mean 0) and "frame drawn" failed; in the full run it was 36, and Windows
  shows 21 to 27 at the same point (its opening film, dark). Read it as the
  film's timing, not a drawing fault.
- Engine messages as in run 1: hunted "Max scene drawsurfs exceed" twice,
  ac130 "Too many visible static models" 5 times, airplane "couldn't find
  path to goal" 3 times.

## Vulkan run 1 (laptop, x86_64, 2026-11-10)

Master 5e2e574, x86_64 RelWithDebInfo with the four Vulkan options and the
singleplayer game, movies on, sound off, `--hold 60 --screenshot`. A local
run beside other laptop work, not CI evidence.

| Level | Back end | Steps | Load | Frame (mean, not black, colours) |
| --- | --- | --- | --- | --- |
| killhouse | DXVK | all seen, exit 0 | 11.7 s | 74, 99 %, 666 |
| cargoship | DXVK | all seen, exit 0 | 16.0 s | 58, 79 %, 671 |
| killhouse | Vulkan | all seen, exit 0 | 45.9 s | 77, 100 %, 677 |
| cargoship | Vulkan | all seen, exit 0 | 19.3 s | 42, 62 %, 594 |
| blackout | Vulkan | all seen, exit 0 | 32.7 s | 54, 100 %, 431 |
| ac130 | Vulkan | all seen, exit 0 | 9.1 s | 107, 99 %, 210 |
| airplane | Vulkan | all seen, exit 0 | 7.2 s | 90, 85 %, 603 |

- No line with "unsupported" or `VK_` in any Vulkan log; each ends with "0
  validation errors, 0 warnings", but validation was off, so that says
  little.
- killhouse's long Vulkan load is the first run: it translated 575 shaders
  (11.2 s) and built 138 pipelines (22.6 s) from nothing; later levels reuse
  the caches.
- cargoship's Vulkan frame is darker than its DXVK frame. One frame each,
  taken at a fixed time in a level with a storm, so it says nothing yet;
  nobody has looked at the images.
- Frame rates (killhouse 25.8, cargoship 30.2, blackout 23.8, ac130 99.3,
  airplane 103.1 fps) ran beside other work on the laptop and are not a
  measurement.
