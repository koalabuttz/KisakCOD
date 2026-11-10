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
   before step 6. Not run yet.

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
- Engine messages worth a later look: hunted prints "Max scene drawsurfs
  exceed" 5 times, ac130 "Too many visible static models" twice, airplane "AI
  couldn't find path to goal" twice. Six levels print the "computer changed"
  profile warning at start: the CPU estimate (`sys_configureGHz`) varies
  between processes by more than the profile's 10 % check. The profile is left
  unchanged.
- Nobody watched the screen. Hand play (step 6) is next.
