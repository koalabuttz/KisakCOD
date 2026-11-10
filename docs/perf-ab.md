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

- `perf_ab "set r_gpuSync 1" "set r_gpuSync 0" 15 4` measures
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

One line per window, a line with that window's busiest threads, then a mean
per variant and, for two variants, the comparison. Every line starts with
`perf:` and the last one is `perf: done`.

```
perf: window 1/8 A: 152.3 fps, frame ms mean 6.566 p50 6.110 p95 12.009 p99 14.385 max 54.861, 2285 frames, cpu ms/frame main 5.612 all 12.403, gpu wait ms/frame main 1.664 render 0.212, battery 11.82 W, gpu 950 MHz, cpu 2210 MHz, 71.0 C
perf:   threads, cpu ms/frame: KisakCOD-mp 5.612, Backend 2.911, dxvk-cs 1.204, ...
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

## Checks

`scripts/tests/test_linux_perf_ab.py` (the `perf_ab` check of
`run_linux_checks.py`) compiles the logic as it is and drives it with a
made-up clock, a made-up game whose speed and power depend on the variant, and
a made-up `/sys` and `/proc` tree: window order, settle time, statistics,
counters across a wrap, the printed lines, and each machine reading. It uses no
game data. The commands themselves need a running game, so they have only run
on machines with the game files; say which when quoting numbers.
