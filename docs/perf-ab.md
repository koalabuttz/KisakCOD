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
perf_phases [1|0]
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
- `perf_phases` is a different kind of count, for the render thread alone:
  see [Where the render thread's time goes](#where-the-render-threads-time-goes-perf_phases).

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
  - `cells: a cell handed to the workers`: inside `scene: cells`, the four
    worker commands queued for each visible cell
    (`R_AddCellSurfacesAndCullGroupsInFrustumDelayed`). What is left of
    `scene: cells` is the walk through the portals.
  - `world: <part>`: parts of `scene: world surfaces`: `lit surfaces`,
    `decals` and `emissive surfaces` (the three walks over the world's
    visible surfaces, each making its batches), `lights and shadow maps` (the
    shadowed lights chosen, the draw method, the sun shadow's maps set up)
    and `a batch's indices copied` (`R_PreTessBspDrawSurfs`, inside the
    three walks and the sun shadow's: the indices of a batch's surfaces
    copied into the frame's index buffer).
  - `static models: <part>`: parts of `scene: static models`: `a model's
    draw surfaces` (`R_SkinStaticModelsCameraForLod`: the visible instances
    of one model made draw surfaces) and, inside it, `cached indices copied`
    (`R_PreTessStaticModelCachedList`). What is left of `scene: static
    models` is the walk over the visible models itself: distance, level of
    detail, lighting slot and cache entry of each. The two copies are timed
    for every batch, hundreds a frame, so on a slow machine take two clock
    readings off each call before comparing them with anything.
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

    Both waits sleep on the worker command event, a millisecond at a time.
    Until 2026-11-15 a thread on Linux, macOS and the PS Vita missed a set of
    that event that another thread reset straight afterwards (the render
    thread and the workers do that every frame) and slept to the end of its
    millisecond, where Windows lets it go at the set
    (`PosixEvent::setCount` in `src/linux/sys_posix.cpp`). On the console
    (trial builds of dc9dc6a7 with a switch between the two, eight windows of
    6 s each at the busy view of mp_crash) `wait for workers: add scene ent`
    went from 1.40 to 0.22 ms a frame and `render: waiting for a frame` from
    0.70 to 0.10. At full size the GPU sets that view's frame and its mean
    stayed at 36.0 ms (p95 37.3 to 36.3); with the 3D picture at half size
    the frame went from 26.59 to 25.74 ms, every window with the fix ahead of
    every window without, and at 0.7 from 26.68 to 26.47.
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
- A clock read costs about 0.7 us there and a timed piece is two, so a run
  slows the frames it measures: the busy view of `mp_crash` at half size ran
  26.0 ms a frame inside a run's windows and 23.2 ms right after it (console,
  2026-11-15). Both variants carry it, so the comparison stays fair; for the
  frame time itself read `vita_statsEvery`'s lines outside a run, and for the
  render thread's split use `perf_phases`, which reads no clock there.

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

## `r_staticCullVec`

For every cell in view the renderer walks the cell's tree of boxes and holds
each box against the cell's clip planes (the view's sides and the portals the
cell is seen through), then each static model and surface of the leaves the
planes cut. The engine's code does that one plane at a time
(`R_AddAabbTreeSurfacesInFrustum_r`). `r_staticCullVec 1` (the default, in
builds with GCC or Clang) walks the same tree with four planes to a sum:
SSE on x86, NEON on 64-bit Arm and on the PS Vita
(`src/gfx_d3d/r_dpvs_static_vec.cpp`). The sums are made in the scalar code's
order, so the same models and surfaces come out;
`scripts/tests/test_static_cull_vec.py` holds the two against each other on
made-up trees, and the world front-end test runs every view both ways.
`r_showAabbTrees` goes back to the scalar walk, which draws its boxes, and so
does `r_dpvs_stats` while it counts: its figures are the scalar walk's.

On the console the counts of run BM (`r_dpvs_stats`, busy view of mp_crash)
were, for one view: 49 cells with 6.4 planes each, 7264 tree nodes with 27075
plane tests, 7119 static models and surfaces with 15499; most leaves that are
cut are cut by three planes or more. `dpvs cell static` took 12.4 ms of CPU a
frame there, half of it on the main thread.

```
perf_ab "set r_staticCullVec 0" "set r_staticCullVec 1"
```

compares the two; `main thread runs: dpvs cell static` and `other threads
run: dpvs cell static` are its timers.

Console run BU (2026-11-15, the PS Vita, that view, eight windows of 10 s):
`dpvs cell static` went from 4.93-4.97 to 4.12-4.15 ms a frame on the main
thread and from 6.20-6.25 to 5.38-5.40 ms on the other threads, and the
process's CPU time from 72.6 to 71.2 ms a frame. The frame itself stayed at
37.3 to 37.4 ms: that view waits on the render thread, not on this work. The
walk got a seventh faster, not twice: most of a box test's time is not the
sums. No other machine has run it.

## `r_fetchAhead`

The scene's walks read small records that lie anywhere in the world's arrays,
one after another, so the processor does not see the next one coming.
`r_fetchAhead 1` (builds with GCC or Clang; on by default on the PS Vita, off
elsewhere) asks for the next ones while one is worked on. Nothing but the time
changes: the same bytes are copied and the same models and surfaces are
marked. It asks for:

- the indices of the next surface of a batch while one surface's indices are
  copied into the frame's index buffer (`R_PreTessBspDrawSurfs`, `r_pretess`),
  and the record of the surface four ahead;
- the cached indices of a static model's next instance
  (`R_PreTessStaticModelCachedList`);
- the record of the visible static model two ahead while one is handed on
  (`R_AddAllStaticModelSurfacesCamera`);
- in the static culling of `r_staticCullVec`, the tree node two further in a
  row of nodes, and in a leaf that planes cut the box of the static model or
  surface two ahead.

```
perf_ab "set r_fetchAhead 0" "set r_fetchAhead 1"
```

compares the two; `main thread runs: dpvs cell static`, `other threads run:
dpvs cell static`, `scene: world surfaces`, `scene: static models` and the
`world:` and `static models:` parts are its timers.

Three console runs on a PS Vita at the busy view of mp_crash are why the Vita
has it on: CB (the CI build of 89c46a25, the index copies alone) and two
trial runs of builds made on the PC from PR 371's head with trial patches, in
which every piece was a bit of its own. The rows are steady to a few hundredths
of a millisecond from window to window. The second trial run held everything
but the tree nodes off against on, eight windows of 6 s in the order A B B A A
B B A, in ms a frame:

| timer | off | on |
| --- | --- | --- |
| `main thread runs: dpvs cell static` | 3.98 to 4.01 | 3.65 to 3.67 |
| `other threads run: dpvs cell static` | 5.54 to 5.57 | 5.18 to 5.21 |
| `scene: world surfaces` (main thread) | 4.54 to 4.56 | 4.05 to 4.07 |
| `scene: static models` (main thread) | 4.61 to 4.64 | 4.01 to 4.02 |

That is 1.8 ms of processor time a frame, 1.45 of it on the main thread. The
tree nodes are another 0.2 ms (3.67 to 3.56 and 5.19 to 5.09 in their own four
windows). The frame was 0.5 to 1.2 ms shorter with it on (34.7 to 35.1 ms
against 35.4 to 36.7): in that build the main thread set the pace at that view.
The whole process's CPU time says less than the rows do, because the sound's
share of the main thread swings by 1.7 ms between windows of either setting.

What the trials ruled out, each in four windows of its own: a leaf's boxes four
ahead instead of two (0.15 ms slower on each of the two threads), indices two
surfaces ahead instead of one (no change), surface records eight ahead instead
of four (0.04 ms slower), the static model record four ahead instead of two (no
change), holding all the nodes of a row before going down into any (0.55 ms
slower on each thread), and holding a leaf's boxes against one or two planes
without the vectors (0.05 ms slower).

No other machine has run it; a desktop processor does this work in a small
fraction of the time, so the setting stays off there until one does.

## `r_mergeLitPreTess`

The world's lit pass draws a pretessellated list piece by piece
(`R_DrawBspDrawSurfsLitPreTess`, `r_draw_bsp.cpp`). The main thread starts a
new piece wherever the first vertex, the lightmap or the reflection probe
changes, and the render thread sets the lightmap and the probe before each
piece. A pass whose shader reads neither (the custom samplers of the probe and
the two lightmaps) draws the same picture whichever are set.
`r_mergeLitPreTess 1` (on by default on the PS Vita, off elsewhere) draws a
piece that changes only a lightmap or probe the pass does not read together
with the piece before it, when both start at the same vertex and the same layer
data. That is how `R_DrawBspDrawSurfsPreTess` already draws the other passes;
the merged draw names the widest vertex range of its pieces.

```
perf_ab "set r_mergeLitPreTess 0" "set r_mergeLitPreTess 1"
```

compares the two; `render: tess pretessellated triangles` and its draws are the
timer.

Console trial J (2026-11-15, a PS Vita, the busy view of `mp_crash`, a build
made on the PC from cbd93105 with the switch as a bit of `r_ignore`) counted
the lit lists on the render thread: 226 lists a frame, 555 pieces, 326 of them
after a change of probe only and 2 after a change of lightmap only. The pass
read neither in all 328 and each started at the vertex of the piece before, so
the lit draws went from 555 to 227 a frame (the back end's from about 1464 to
1136). At half the 3D size, where the render thread sets the pace, the frame
went from 25.24 to 23.68 ms (every window with it on faster than every window
with it off; the windows of one setting differ by 0.4 %), and the render
thread's draw phase (`perf_phases`) was 1.90 ms shorter. At full size the
graphics chip sets the pace: 30.96 to 30.65 ms, with 2.3 ms less processor
time a frame.

The probe is what changes, and the Vita's lit shaders do not read it. On a
machine whose lit shaders read the probe (specular on), only lightmap changes
can merge, so the setting stays off there until a run shows what it is worth.

## Where the render thread's time goes (`perf_phases`)

The timed pieces above read a clock twice each. That is too dear for what the
render thread does a few thousand times a frame, per draw and per constant:
on the Vita the clock reads would outweigh the work. `perf_phases` counts
without a clock. The render thread writes one byte whenever it enters or
leaves a marked piece of work (`SysPerfPhaseScope`, `src/linux/sys_perf.h`;
the innermost mark is the one that stands), and a thread of high priority
looks at that byte a few thousand times a second and counts what it sees. A
phase's share of the looks is its share of the thread's time.

```
perf_phases 1      start a count
perf_phases 0      end it and print
```

The first lines of a count (PS Vita, see below):

```
perf: render thread phases, 17891 looks in 22.00 s (one each 1230 us), 941 frames (23.38 ms a frame)
perf: phase device: draw: 54.76 %, 12.803 ms a frame, 1464.0 calls a frame, 8.75 us a call
perf: phase tess pretessellated triangles: 5.10 %, 1.192 ms a frame, 380.0 calls a frame, 3.14 us a call
perf: phase device: vertex constants: 4.65 %, 1.087 ms a frame, 1174.0 calls a frame, 0.93 us a call
...
```

- The time is a phase's own: work marked inside it is counted under its own
  name. **calls a frame** counts entries, and **us a call** is the phase's own
  time over them.
- The marks are compiled in only in Debug builds and in a build configured
  with `-DKISAK_PERF_PHASES=ON`; measure with the second, which is a release
  build otherwise. Elsewhere a mark is nothing and the command says so.
- The looks are asked for every 397 us; a busy console gave one each 1.2 ms.
  Twenty seconds is some 16 000 looks, which holds a 1 % share to about a
  tenth of itself. Two counts of the same view agreed within 0.4 points, and
  the frame was as long with a count on as without (23.4 ms).
- The phases: `waiting for a frame`, `present`, `3d outside the lists`, `2d`
  and `2d text`, `a filled batch drawn` (2D, effects and code meshes flushed),
  `list walk`, `material setup`, `pass setup`, `a pass's textures and
  constants`, `critical args`, `per-object args`, `per-prim args`, `matrices
  derived`, `model placement`, `lighting coords`, `state bits`, `vertex
  declaration`, `index fill`, `vertex fill`, one `tess <surface type>` for each
  kind of surface (the kind's own loop, outside all of the others), and the
  graphics device's part: `device: draw`, `device: vertex constants`, `device:
  pixel constants`, `device: textures`, `device: streams`, `device: shaders`.

A count on a PS Vita (console, the busy view of `mp_crash` at
`1264 -1896 124` yaw 90, twelve bots, `vita_sceneScale 0.5` so that the
processor and not the graphics chip sets the frame; this commit built with
the option, 17 891 looks in 22 s, 23.38 ms a frame, 2026-11-15). A second
count right after it gave the same within 0.09 ms on every line.

| Phase | Share | ms a frame | Calls a frame | us a call |
| --- | --- | --- | --- | --- |
| device: draw | 54.8 % | 12.80 | 1464 | 8.75 |
| tess pretessellated triangles (the world) | 5.1 % | 1.19 | 380 | 3.14 |
| device: vertex constants | 4.7 % | 1.09 | 1174 | 0.93 |
| device: textures | 3.9 % | 0.90 | 850 | 1.06 |
| material setup | 3.2 % | 0.74 | 710 | 1.05 |
| device: streams | 2.7 % | 0.63 | 912 | 0.69 |
| tess rigid models | 2.6 % | 0.60 | 24 | 25.0 |
| 2d text | 2.3 % | 0.53 | 25 | 21.2 |
| a pass's textures and constants | 2.1 % | 0.48 | 560 | 0.86 |
| 3d outside the lists | 1.9 % | 0.45 | 1 | 451 |
| per-prim args | 1.8 % | 0.43 | 1463 | 0.29 |
| pass setup | 1.7 % | 0.41 | 702 | 0.58 |
| 2d | 1.4 % | 0.34 | 1 | 336 |
| device: shaders | 1.2 % | 0.28 | 259 | 1.06 |
| state bits | 1.1 % | 0.26 | 121 | 2.15 |
| critical args | 1.0 % | 0.24 | 702 | 0.35 |
| vertex fill | 1.0 % | 0.24 | 32 | 7.51 |
| eighteen more, each under 1 % | 7.6 % | 1.77 | | |

So more than half of that thread is inside the draw call of the Vita's
graphics back end, an eighth in the other device calls and a third (7.6 ms)
in the engine's own code, spread thin: its largest piece is the loop over the
world's batches. Deriving matrices is 0.5 %: replacing the 4x4 products there
with vector code (tried on the console as `r_ignore` in a trial build)
changed nothing that a run could show, and was dropped.

At full size the same view waits for the graphics chip, and the wait shows
inside the draw call: 33.61 ms a frame, device: draw 72.4 % (16.6 us a call
against 8.75), present 2.5 %, every other phase about as long as at half
size or a little shorter.

## Measured on the PS Vita and left alone

Trial builds on the console (2026-11-15, the busy view of `mp_crash` at full
size, each switch a bit of `r_ignore` in a build that was never merged, A B B A
windows of 6 s). They are listed so that nobody tries them again without a new
reason. The frame is the graphics chip's at that size, so the figure is the
main thread's time in a frame, which windows of one variant repeat to 0.05 ms.

- **The world's index copies** (539 draws a frame from 2522 surfaces, 169 008
  indices; another 141 048 for 776 static models). Not copying the world's at
  all, for measuring only, saves 1.8 ms: that is all a cache of them across
  frames could ever save. Copying by a loop of words in place of `memcpy` is
  slower (the world +0.35 ms, the static models +1.06 ms). Copying surfaces
  whose indices follow each other with one call saves 0.13 ms. The draws
  already equal the groups of surfaces that may share one, so regrouping
  cannot lower their number.
- **Static culling's cost is work, not waiting for memory.** Walking every
  cell's tree a second time and marking nothing costs 3.3 ms of main thread
  (6.5 ms over all threads). Making a node's sums twice costs
  1.2 ms and a model's or surface's sums twice 0.6 ms, so the sums are over
  half of a walk and what is left for fetches and branches is about 1.4 ms.
  `r_fetchAhead` already took what fetching ahead can.
- **More static models in the model cache.** The cache (4 MB on the Vita)
  is already full every frame (4088 of 4096 KB), so giving more models places
  in it at map load only moves models in and out.
- **Rebasing the world's indices** so that pieces at different first vertices
  share a draw. The main thread already splits a list only where the lightmap
  or probe changes; rebasing left the 539 lit draws at 539.
- **The 4x4 matrix products of a draw** are 401 instructions each there (sums
  in double precision), but deriving matrices is 0.4 % of the render thread
  (see above). Products of four floats at a time changed the frame by
  +0.04 ms, inside what windows of one variant differ by.

## Checks

`scripts/tests/test_linux_perf_ab.py` (the `perf_ab` check of
`run_linux_checks.py`) compiles the logic as it is and drives it with a
made-up clock, a made-up game whose speed and power depend on the variant, and
a made-up `/sys` and `/proc` tree: window order, settle time, statistics,
counters across a wrap, the printed lines, each machine reading, the timed
pieces (fed from four threads), and the phase marks with the lines of a count. It uses no game data. The commands themselves need a running game, so they have only run
on machines with the game files; say which when quoting numbers.
