# Measuring a change from inside the game (`perf_ab`)

`perf_ab` compares two settings in one launch of the game. It exists because a
measuring run on a real machine is expensive: on the laptop a launch costs
battery, the machine is shared between threads of work, and its speed drifts
by more from one launch to the next than most changes are worth (perf run 4:
the drift was +8.6 fps per launch, the change under test +5.4).

The commands are in every build: the native builds that use `src/linux` (the
Linux client, the dedicated server, singleplayer, macOS, the Vita) and the
Windows builds (MSVC and MinGW: client, dedicated server, singleplayer). What
Windows reads differs in places; see [On Windows](#on-windows).

## Commands

```
perf_ab "<commands A>" "<commands B>" [seconds] [rounds] [settle seconds]
perf_sample [seconds] [windows] [settle seconds]
perf_stop
```

- `perf_ab "set r_gpuSyncSleep 0" "set r_gpuSyncSleep 100" 15 4` measures
  eight windows of 15 s: A B B A A B B A. A and B are console command lines,
  so one variant can set several things (`"set a 1; set b 2"`).
- `perf_sample 20 3` measures three windows of 20 s without changing anything.
- Defaults: 10 s windows, 4 rounds for `perf_ab` and 1 window for
  `perf_sample`, 2 s (`perf_ab`) or 1 s (`perf_sample`) to settle.
- A last number after the settle time is how many timed pieces a window
  lists (16 by default): `perf_sample 15 2 1 48` lists the 48 with the most
  time, sixteen to a line.
- A run leaves the last window's variant applied. With an even number of
  rounds that is A.

Each window applies its variant, waits the settle time without recording, then
records every frame for the sample time. The order A B B A puts both variants
equally early and late in the run, so a drift that is steady over the run (the
machine warming up, the battery running down) falls on both alike. The second
window of a pair does not apply its commands again but still waits to settle.

## What it prints

One line per window, a line with that window's busiest threads, a line with
the timed pieces of the frame, on a listen server a line that splits the frames
by whether the server ran in them, then a mean per variant and, for two
variants, the comparison. Every line starts with `perf:` and the last one is
`perf: done`. The numbers in this example are made up.

```
perf: window 1/8 A: 152.3 fps, frame ms mean 6.566 p50 6.110 p95 12.009 p99 14.385 max 54.861, 2285 frames, cpu ms/frame main 5.612 all 12.403, gpu wait ms/frame main 1.664 render 0.212, battery 11.82 W, gpu 950 MHz, cpu 2210 MHz, 71.0 C
perf:   threads, cpu ms/frame: KisakCOD-mp 5.612, Backend 2.911, dxvk-cs 1.204, ...
perf:   timed, ms/frame (calls/frame, longest ms): hud 0.880 (1.00, 1.420), server frame 0.750 (0.13, 7.310), wait for workers: skin ent delayed 0.410 (0.62, 1.105), lock CONSOLE 0.004 (2.00, 0.012), ...
perf:   301 frames with a server frame: frame ms mean 11.204 p50 10.870 p95 14.911 max 54.861; the other 1984: mean 5.862 p50 5.790 p95 8.004 max 15.220
perf: A mean of 4: ...
perf: B mean of 4: ...
perf: B against A: fps +3.0 %, frame -0.190 ms, cpu all -1.100 ms/frame, battery -0.90 W; windows of one variant differ by up to 1.4 %
perf: done
```

- **Frame times** are the time between two passes of the main loop.
  Percentiles are nearest rank.
- **cpu ms/frame** is CPU time used per frame: `main` by the main thread, `all`
  by every thread of the process, the driver's included. A thread that spins
  while it waits counts in full.
- **gpu wait ms/frame** is how long the main thread stayed in `R_SyncGpu`
  (which includes work it does while it waits) and how long the render thread
  waited for the previous frame's fence at the end of a frame. Both are 0 with
  `r_gpuSync 0` and in builds without a renderer.
- **threads** comes from `/proc/self/task/*/schedstat` (on Windows, see
  below). Threads of one name are
  added up.
- **timed** lists pieces of the frame that are timed while a run is active,
  the sixteen with the most time (or as many as the command's last number
  asks for, sixteen to a line): per frame, calls per frame, and the longest
  single call of the window.
  - `server frame`: one frame of a server (20 a second) with its sends. Its
    time per server frame is its ms/frame divided by its calls/frame.
  - `server: <part>`: the parts of a server frame, inside `server frame`:
    `bot moves` (`SV_UpdateBots`, the bots' `ClientThink`), `triggers`
    (players touching triggers, then the trigger notifies and their scripts;
    two calls a server frame), `animation` (`G_RunFrame`'s two passes over
    every entity's animation; two calls), `scripts` (`Scr_IncTime`: the script
    threads whose wait ended), `entities` (`G_RunFrameForEntity` for every
    entity), `player end frame` (`ClientEndFrame`) and `snapshots`
    (`SV_SendClientMessages`). What a server frame does besides these
    (pings, configstrings, objectives, hud elements, team status) is the
    rest of `server frame`.
  - `hud`: `CG_Draw2D`.
  - `hud: <part>`: parts of the HUD, inside `hud`: `menus` (`Menu_PaintAll`,
    the HUD's menus), `script elements` (`CG_Draw2dHudElems`, two calls a
    frame), `crosshair and names` (`CG_ScanForCrosshairEntity` and
    `CG_UpdatePlayerNames`), `compass` (`CG_CompassUpdateActors`) and
    `scoreboard` (`CG_DrawScoreboard`).
  - `menu expressions`: `EvaluateExpression`, every menu expression
    (`visible when`, `exp text` and the like), in the HUD or in any other
    menu.
  - `menus: <part>`: parts of painting a menu, in the HUD or in any other
    menu, so the time outside the expressions can be split: `windows`
    (`Window_Paint`, a menu's or an item's background and border),
    `text items` (`Item_Text_Paint`, with its translation and its text
    expression), `ownerdraw items` (`Item_OwnerDraw_Paint`, what the game
    draws itself: ammo, compass and the like), `other items` (every other
    item type), `translate` (`UI_SafeTranslateString`, a `@NAME` text looked
    up), `materials by name` (a background material found by its name each
    frame) and `show by dvar` (`Item_EnableShowViaDvar`, an item shown or
    hidden by a dvar's value). The parts overlap: `translate` and the text
    expressions sit inside `text items`. On a slow machine each timed call
    adds two clock readings, so compare the parts with each other, not with
    a run without them.
  - `client: <part>`: the main thread's client frame (multiplayer client).
    `frame` is `CL_Frame` (packets in and out). `screen` is
    `SCR_UpdateScreen` and holds the others and `hud`: `snapshots`
    (`CG_ProcessSnapshots`), `entities` (`CG_AddPacketEntities`, every
    entity of the snapshot), `input and prediction` (`CL_Input`,
    `CG_PredictPlayerState`), `view and weapon` (the view's values, the view
    weapon, the player's own entity), `scene` (`CG_DrawActive`:
    `R_RenderScene` with its waits for workers), `sound` (`CL_UpdateSound`,
    inside `scene` in a map), `2d` (menus over the game, the console) and
    `hand over the frame` (`R_EndFrame` and `R_IssueRenderCommands`, which
    holds `wait for the render thread`).
  - `scene: <part>`: the main thread's own parts of the scene, inside
    `client: scene`: `cells` (`R_AddWorldSurfacesDpvs`: the visible cells,
    handed to the workers), `world surfaces` (the world's surfaces added and
    the sun shadow's maps set up), `static models`
    (`R_AddAllStaticModelSurfacesCamera`), `entities`
    (`R_DrawAllSceneEnt`) and `sorted lists`
    (`R_MergeAndEmitDrawSurfLists`, `R_SortDrawSurfs`).
  - `workers: awake`: a worker thread from waking to its next sleep, both
    workers added up. Its calls are wake-ups; a wake-up that finds no
    command still counts.
  - `main thread runs: <type>` and `other threads run: <type>`: worker
    commands of that type as they run (`R_ProcessWorkerCmdInternal`), by who
    ran them. The main thread runs commands while it waits, so its share is
    inside its `wait for workers` time; the other threads are the two
    workers and the render thread. A command that waits for commands of
    another type holds their time too.
  - `wait for workers: <type>`: the main thread asleep until the worker
    threads finish the commands of that type; `all` is the wait for every
    front-end command before the frame is handed over, `other` any other.
  - `wait for the render thread`: the main thread asleep until the render
    thread takes the frame.
  - `render: <part>`: the render thread's frame. `3d` is `RB_Draw3D`, the
    3D views, and holds `depth pass` (`R_DepthPrepass`), `lit`
    (`R_DrawLit`), `decals`, `sun`, `point lights`, `emissive` (effects and
    blended surfaces) and `post effects` (`RB_EndSceneRendering`). `2d` is
    the HUD, menus and other 2D commands; `wait before present` the wait for
    the swap fence (worker commands run in it); `present` is `RB_EndFrame`,
    which on the Vita holds the wait for the frame before last; `waiting
    for a frame` is the render thread asleep until the main thread hands it
    one (worker commands run in it too). The 3D parts time the engine's
    own work and the back end's draws together; on the Vita the GXM back
    end's stats line gives its draws' share.
  - `render: material setup` and `render: tess <surface type>`: inside the
    3D parts, each run of surfaces of one material: the material's and its
    pass's setup (`R_SetupMaterial`, `R_SetupPass`: state, textures,
    constants), then the surface type's function from `rb_tessTable`
    (static models, brush models, skinned models, triangles and so on) with
    the draws it makes. The calls a frame count the runs. `render: draws by
    tess <surface type>` is the same time again with the draws the runs
    made as its calls, so its time over its calls is what one draw of that
    kind costs with the engine's work for it; runs that drew nothing (their
    surfaces only went into a batch that a later run draws) are left out,
    and a batch's draw counts for the run that flushed it. These take two
    clock readings a run, which on the Vita (about 0.7 us each) adds about
    a millisecond to a busy frame while a run is active.
  - `lock <name>`: any thread getting into that critical section. The time is
    the wait for the lock plus two readings of the clock (about 50 ns).
  Outside a run each of these costs one load and one branch.
- **frames with a server frame** compares the frames in which the server ran
  with the others. With bots on a listen server the server's frame lands in
  one frame in several, so it shows as uneven frames more than as a lower
  mean. The line is left out when no server frame ran (a client on a remote
  server).
- **battery**, **package**, **gpu MHz**, **cpu MHz** and the temperature are
  read four times a second from sysfs where the machine has them and the user
  may read them: `power_supply/BAT*/power_now` (or current times voltage),
  `powercap/intel-rapl:0/energy_uj` (root only on most systems), the i915 or
  xe driver's current GPU frequency, `scaling_cur_freq` averaged over the
  CPUs, and the `x86_pkg_temp` thermal zone. A reading that is missing is left
  out of the line. Battery power only means something on battery; a laptop's
  controller updates it every second or two, so use windows of 10 s or more
  when power is the question.
- **windows of one variant differ by up to** is the yardstick: a difference
  between A and B smaller than that is not shown by this run.

## On Windows

The same commands, lines and probes, with these differences:

- **cpu ms/frame** comes from `GetThreadTimes` and `GetProcessTimes`. Windows
  counts these in whole scheduler ticks (about 15.6 ms), so a window gathers
  a tick's error at most per thread: use windows of 10 s or more.
- **threads** lists the threads of a Toolhelp snapshot of the process, with
  the same CPU times. The engine's own threads carry its names (`Main`,
  `Backend`, `Worker0`, ...); every other thread, the graphics driver's and the
  sound library's among them, is added up as `other`.
- **gpu wait ms/frame** counts the same two waits, but without `r_gpuSyncSleep`
  (Linux only) both always spin.
- No machine readings (battery, package energy, frequencies, temperature).

The Windows PC is a desktop with a GPU much faster than its CPU, so there a
frame's time is mostly the CPU's and what the CPU saves shows as frames per
second, which a laptop that waits for its GPU does not show.

## On the PS Vita

- **cpu ms/frame** is the time the kernel counted each thread as running
  (`sceKernelGetThreadInfo`, `runClocks`). A thread that waits for a lock, a
  file or another thread does not run, so `main` against the frame's length
  says how much of a frame the main thread worked and how much it waited.
- **threads** lists the threads the port starts, by the engine's names
  (`MAIN`, `BACKEND`, `WORKER0`, ...) plus `sound output` and `gxm programs`:
  an application cannot list its threads, so each says its name once when it
  starts (`Vita_NoteThread`, `src/vita/vita_threads.cpp`). The system's own
  threads in the process (sceGxm's display queue, the audio ports) are not in
  the list, and `all` is the sum of the list.
- The Vita gives an application three cores: a table that adds up to less
  than two frames' worth of time leaves a core free.
- No machine readings.

## `r_gpuSyncSleep`

With `r_gpuSync 1` (the default) the engine waits for the GPU in two places:
the main thread before it starts a frame (`R_SyncGpu`), and the render thread
at the end of a frame (`RB_AdaptiveGpuSyncFinal`). Both waits keep a core busy.
When the GPU is the limit they are most of the frame: at 1920x1200 on the
laptop the main thread waited 9 ms of a 14.8 ms frame (perf run 1). On a
laptop the CPU cores and the GPU share one power budget.

`r_gpuSyncSleep <microseconds>` (non-Windows builds) makes both waits sleep
that long between checks of the fence. A wait then ends up to one sleep late.
`r_gpuSyncSleepThreads` picks the waits it applies to (1 the main thread's, 2
the render thread's, 3 both, the default), to tell their effects apart. 0
keeps a core busy as before.

The default is 100 on Linux and 0 elsewhere. Laptop perf run 5 (Intel U300,
battery, power saver, mp_crash with 12 bots, `perf_ab` 0 against 100 in one
launch) measured:

| | fps | CPU ms/frame, whole game | battery |
|---|---|---|---|
| 1280x800 windowed | +2.8 % | -2.75 | -0.54 W |
| 1920x1200 fullscreen | +12.6 % | -8.25 | -0.23 W |

50 and 200 were no better than 100. Vita and macOS have had no such run.

### `r_gpuSyncSleepUnlocked`

`R_SyncGpu` holds the GPU fence lock for the whole of its wait. The render
thread takes the same lock at the end of each frame to issue the next fence,
so it waits for the main thread's wait to end. `lock GPU_FENCE` in the
`timed` line is the time both threads spend getting the lock (the main
thread also waits while the render thread holds it for its own wait): 0.9 to
1.2 ms/frame at 1280x800 and 2.5 to 3.7 ms/frame at 1920x1200 in run 5.
Which thread pays how much is not measured. `r_gpuSync 0`, which has no waits and no lock, gained
16.6 % fps at 1280x800 but gives up the limit on how far the CPU runs ahead
of the GPU (mouse latency).

`r_gpuSyncSleepUnlocked 1` (default 0) keeps that limit: the main thread lets
go of the lock while `r_gpuSyncSleep` sleeps, and its wait ends when the fence
has passed, as before, or when the render thread has issued a new fence. The
render thread issues one only after the previous fence passed, so the wait
never ends sooner than before. It does nothing with `r_gpuSyncSleep 0` or
`r_gpuSyncSleepThreads 2`. It stays off until a laptop run compares it.

## `xanim_blendOnce`

When an animation tree node blends several children, `XAnimCalc` calculated
the first of them twice in a row with the same arguments, and so on down the
tree: a blend inside the first child of a blend ran four times. The second
calculation clears and writes the same bones from the same state, so it should
change nothing.

`xanim_blendOnce` (default 0 = unchanged) picks what happens: 0 calculates
twice, 1 once, 2 twice and compares the two results bone by bone. With 2 the
console prints `xanim_blendOnce 2: <n> blends calculated twice, <m> differed`
at 1024 blends and at every doubling after that, and a warning for each of
the first eight that differ. The setting stays at 0 until a run on real
animations has shown 0 differed; `perf_ab "set xanim_blendOnce 0" "set
xanim_blendOnce 1"` measures what it saves.

## `ui_fastExpressions`

Menu expressions (`visible when`, `exp text`, `exp rect` and the like) run
every frame for every HUD item, and `menu expressions` in the timed line
shows what they cost. Two pieces of that work give nothing:
`EvaluateExpression` clears its whole 5 KB operand stack before each
expression, and `dvarInt`, `dvarBool` and `dvarFloat` format the dvar's value
as text and parse the text back.

`ui_fastExpressions` (default 1; 0 = the retail way) at 1 clears only what can be
read before it is written, reads bool, int and string dvars directly (float,
vector, colour and enum dvars keep the text, whose `%g` rounding or enum name
decides the result), and lets `dvarString` use the dvar it already found
instead of looking it up again. Every result is the same as with 0:
`scripts/tests/test_ui_fast_expressions.py` compares the two dvar reads bit
for bit, and every operand list is written before it is read except
`stack[0]` in the error path of a malformed expression, which the fast path
clears as the memset did.
`perf_ab "set ui_fastExpressions 0" "set ui_fastExpressions 1"` measures what
it saves. On the PC (Windows, 12 bots on mp_crash) `menu expressions` went from
0.111-0.118 to 0.094-0.098 ms a frame; on the Vita at mp_crash's busiest view
from 2.88 to 2.21 ms a frame. Neither changed the frame rate beyond the spread
(the PC's HUD is small, and the Vita waits for its render thread), so 1 is the
default because it does less work for the same results.

## `ui_exprStats`

`ui_exprStats [seconds]` (10 s if left out, 1 to 600) times every operator of
every menu expression for that long, then prints:

```
exprstats: 10.0 s, 601 frames, 124.0 expressions a frame, 6.31 entries each, 2.203 ms a frame in all, one clock reading 0.412 us
exprstats:   dvarbool: 96.00 a frame, 0.4100 ms a frame, 4.27 us each
exprstats:   ...
exprstats:   outside the operators: 0.5210 ms a frame
exprstats: done
```

Operators are listed by time, most first, by the name they have in a menu
file (`dvarint`, `tablelookup`, `&&` ...). A `)` runs the operators it closes
and is not listed itself. The clock is read twice around each operator, so an
operator's time holds about one reading of the clock and `outside the
operators` holds two for each operator; `one clock reading` says what to take
off. It prints numbers only, never an expression's text. Outside a match
there is no frame count and the figures are per second. While it is off it
costs one load and a branch per operator.

## `r_dpvs_stats`

A perf window times the culling of a cell's models and surfaces (`main thread
runs: dpvs cell static` and `other threads run: dpvs cell static`) but cannot
say what the time goes on. `r_dpvs_stats` counts it: the first call starts
counting and the second prints four lines, all per view walked:

- cells culled and the planes each started with; the nodes of the cells' box
  trees that were visited, the plane tests made on them (each tests two
  corners of the box), and how many nodes were outside, wholly inside (their
  models and surfaces are marked with no further test) or leaves cut by a
  plane;
- those cut leaves by the number of planes left at them;
- for static models and for surfaces: how many were marked with their leaf,
  had been seen before through another cell, or were tested, with how many
  plane tests and how many of the tested were culled; and the cull groups
  tested.

The walk is `R_AddAabbTreeSurfacesInFrustum_r` (`src/gfx_d3d/r_dpvs_static.cpp`);
the counts are for deciding what a faster box test is worth there. While it is
off it costs a load and a branch per node.

## Checks

`scripts/tests/test_linux_perf_ab.py` (the `perf_ab` check of
`run_linux_checks.py`) compiles the logic as it is and drives it with a
made-up clock, a made-up game whose speed and power depend on the variant, and
a made-up `/sys` and `/proc` tree: window order, settle time, statistics,
counters across a wrap, the printed lines, each machine reading, and the timed
pieces (fed from four threads). It uses no game data. The commands themselves need a running game, so they have only run
on machines with the game files; say which when quoting numbers.
