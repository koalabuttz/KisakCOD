# Measuring a change from inside the game (`perf_ab`)

`perf_ab` compares two settings in one launch of the game. It exists because a
measuring run on a real machine is expensive: on the laptop a launch costs
battery, the machine is shared between threads of work, and its speed drifts
by more from one launch to the next than most changes are worth (perf run 4:
the drift was +8.6 fps per launch, the change under test +5.4).

The commands are in every native build that uses `src/linux` (the Linux
client, the dedicated server, singleplayer, macOS, the Vita). The Windows
build does not have them yet.

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
- **threads** comes from `/proc/self/task/*/schedstat`. Threads of one name are
  added up.
- **timed** lists pieces of the frame that are timed while a run is active,
  the twelve with the most time: per frame, calls per frame, and the longest
  single call of the window.
  - `server frame`: one frame of a listen server (20 a second) with its sends.
  - `hud`: `CG_Draw2D`.
  - `wait for workers: <type>`: the main thread asleep until the worker
    threads finish the commands of that type; `all` is the wait for every
    front-end command before the frame is handed over, `other` any other.
  - `wait for the render thread`: the main thread asleep until the render
    thread takes the frame.
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

## `r_gpuSyncSleep`

With `r_gpuSync 1` (the default) the engine waits for the GPU in two places:
the main thread before it starts a frame (`R_SyncGpu`), and the render thread
at the end of a frame (`RB_AdaptiveGpuSyncFinal`). Both waits keep a core busy.
When the GPU is the limit they are most of the frame: at 1920x1200 on the
laptop the main thread waited 9 ms of a 14.8 ms frame (perf run 1). On a
laptop the CPU cores and the GPU share one power budget.

`r_gpuSyncSleep <microseconds>` (non-Windows builds, default 0 = unchanged)
makes both waits sleep that long between checks of the fence. A wait then ends
up to one sleep late. `r_gpuSyncSleepThreads` picks the waits it applies to
(1 the main thread's, 2 the render thread's, 3 both, the default), to tell
their effects apart. The sleep is off until a laptop run shows what it does
to frame rate and power; `perf_ab` was written to measure it.

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

## Checks

`scripts/tests/test_linux_perf_ab.py` (the `perf_ab` check of
`run_linux_checks.py`) compiles the logic as it is and drives it with a
made-up clock, a made-up game whose speed and power depend on the variant, and
a made-up `/sys` and `/proc` tree: window order, settle time, statistics,
counters across a wrap, the printed lines, each machine reading, and the timed
pieces (fed from four threads). It uses no game data. The commands themselves need a running game, so they have only run
on machines with the game files; say which when quoting numbers.
