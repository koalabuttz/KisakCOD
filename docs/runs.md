# Runs on the test machines

Some checks need a GPU, the game's data or a console: frame-rate comparisons,
picture comparisons, Vita3K and PS Vita starts. They run by hand or by script
on a machine that has those, never on hosted CI, and their results are local
evidence, not CI evidence. This page collects what makes such a run short and
repeatable. Nothing here reads or copies game files, and no run commits a
screenshot, a log or a binary.

## Frame rate without a profiler: `framelog`

`framelog <seconds> [label]` on the console records the time from each
`Com_Frame` to the next for that long and then prints one line:

    frame log A-1: 16928 frames in 120.004 s: 141.06 fps; ms mean 7.089, median 6.877, p95 11.126, p99 12.878, max 159.429; 1 over 50 ms

It is in every build (client and dedicated server, every platform) and needs
no profiler attached, so a frame-rate row costs its measuring time and nothing
after it. A profiler capture is still the tool when a run asks for named zones
or per-thread CPU time.

- The recording starts with the next frame and announces itself (`frame log
  A-1: recording 120.000 s`).
- The label (up to 31 characters, no spaces) keeps the lines of several
  recordings apart.
- `framelog stop` ends a recording early; the line then ends `(stopped)`. A
  `quit` during a recording prints what was recorded, ending `(at the quit)`.
- The percentiles use the nearest-rank rule on the sorted frame times: `p95` is
  the smallest time with at least 95% of the frames at or below it. `over 50
  ms` counts frames longer than 50 ms.
- The frame rate is frames divided by the recorded time, so it is the same
  figure a capture's frame count over its length gives.

The arithmetic is `src/qcommon/com_frame_log.h`, checked on the host by
`scripts/tests/test_frame_log.py`.

On the native Linux client the console is the process's standard input, so a
driver sends `framelog 120 A-1` after its settle time and waits for the line.
On the PS Vita the timed commands do it: `+set vita_atMap "20 framelog 30
map"`.

## A frame-rate run from one recipe: `perf_run.py`

`scripts/runs/perf_run.py` is the driver for frame-rate runs of the native
client. A run used to be a set of shell scripts copied and edited per run
(launcher, scene steps, driver, export, guard) and a report typed by hand; now
it is one recipe file, and the driver writes the report.

    python3 scripts/runs/perf_run.py recipe.json --out ~/kisak-private/run-11
    python3 scripts/runs/perf_run.py recipe.json --out ~/kisak-private/run-11 --resume
    python3 scripts/runs/perf_run.py recipe.json --out /tmp/x --check

The recipe (format `kisak-run/1`, JSON) names the game binary with its
arguments and environment, the variants to compare, the order of the launches,
the scene as console lines, and the log lines wanted:

    {
      "format": "kisak-run/1",
      "id": "vulkan-laptop-run-11",
      "title": "Vulkan back end, laptop run 11: the feed interval",
      "asker": "Develop the Vulkan renderer",
      "game": {
        "binary": "~/kisak-build/vk-mp-x64/bin/KisakCOD-mp",
        "args": ["+set", "fs_basepath", "~/games/cod4", "+set", "com_maxfps", "0"],
        "args_after": ["+devmap", "mp_crash"],
        "env": {"LD_LIBRARY_PATH": "~/kisak-sdk/dxvk-x64/lib:~/kisak-sdk/sdl3-x64/lib"},
        "clear_env": ["KISAK_VK_*", "TRACY_*"],
        "home_template": "~/kisak-private/home-template"
      },
      "variants": {
        "A": {"args": ["+set", "r_backend", "vulkan"]},
        "D": {"args": ["+set", "r_backend", "d3d9"]},
        "F64": {"args": ["+set", "r_backend", "vulkan"], "env": {"KISAK_VK_GPU_FEED_DRAWS": "64"}}
      },
      "rows": ["A", "D", "F64", "F64", "D", "A"],
      "scene": [
        {"wait_quiet": 15, "min": 20, "timeout": 150},
        {"send": "sv_serverid"},
        {"expect": "\"sv_serverid\" is: \"(?P<serverid>\\d+)", "timeout": 10},
        {"send": "mr {serverid} 8 autoassign"},
        {"sleep": 3},
        {"send": "mr {serverid} 15 assault_mp,0"},
        {"sleep": 5},
        {"send": ["god", "setviewpos -716 461 295 121", "viewpos"], "gap": 1},
        {"expect": "^\\(-716 461 ", "timeout": 5},
        {"send": "set scr_testclients 12"},
        {"poll": "status", "count": "bot\\d+\\^7", "at_least": 12, "timeout": 120, "name": "bots"},
        {"sleep": 10},
        {"framelog": 120}
      ],
      "lines": ["^Vulkan: legacy: device ready:", "frame stats:"],
      "power": {"require": "same", "battery_floor": 10, "battery_per_launch": 8,
                "profile": "power-saver", "wait": 300, "retries": 1},
      "load": {"max_cpu_percent": 50, "wait": 600},
      "stop_if_running": ["Vita3K"],
      "guard": ["~/games/cod4", "~/games/cod4/main", "~/games/cod4/players/**"]
    }

Rows are named by variant and launch (`A-1`, `D-1`, `F64-1`, `F64-2`, `D-2`,
`A-2`). The script's own header describes every key and scene step.

Recipes that are run more than once live in `scripts/runs/recipes/`, so a
machine fetches a branch and runs one line instead of taking pasted steps. They
name no machine's folders: they write `{game}`, `{build}`, `{libs}`,
`{home_template}`, `{mesa_cache}` and `{dxvk_prefix}`, and each machine gives
those once in a names file (`~/.config/kisak/run-names.json`, or the file
`--names` or the environment variable `KISAK_RUN_NAMES` gives):

    {
      "game": "~/games/cod4",
      "build": "~/kisak-build/vk-mp-x64",
      "libs": "~/kisak-sdk/dxvk-x64/lib:~/kisak-sdk/sdl3-x64/lib",
      "home_template": "~/kisak-private/home-template",
      "mesa_cache": "~/kisak-private/mesa-cache",
      "dxvk_prefix": "~/kisak-sdk/kisak-dxvk-prefix"
    }

`--set build=~/kisak-build/pr-160` overrides one name for one run, which is
how a run picks the build of the branch it tests. A variant may name a
`"binary"` of its own, so one run compares two builds, and every row notes the
SHA-256 and the time of the binary it ran. A recipe that uses a name
nobody gave is refused before anything starts, and `--check` says whether the
game binary is there. `scripts/runs/recipes/vulkan-ab-mp-crash.json` is the
Vulkan back end's standing comparison: a listen server on mp_crash, a fixed
view, 12 bots, a slow turn and 120 seconds measured.

What the driver does by itself, because each has spoiled a row before:

- **Power.** `require` is `any`, `same` (every row in the state the first one
  had), `mains` or `battery`. Before a launch it waits up to `wait` seconds for
  that state, and stops the run when the battery is under `battery_floor` plus
  `battery_per_launch`, or the power profile is another one. During a capture
  it reads the charger once a second; a row whose power changed is kept, marked
  `.spoiled1`, left out of the averages and run again (`retries`). A row
  during which the machine slept (a closed lid: the game wakes up and carries
  on, with wrong waits and frame times) is treated the same way, and says how
  many seconds it slept.
- **Other load.** Before a launch (`wait`) and again before a capture
  (`wait_at_capture`, 120 seconds unless `wait` is shorter) it waits while
  another process uses more than `max_cpu_percent` of a core, and it measures
  the other processes over the capture itself. A row captured beside a
  process over the limit is kept, marked `flagged: other load during the
  capture (java 234% of a core)` and run again like a row whose power changed.
  The first run on the laptop showed why: a process that was not ours started
  inside a row, the row lost a sixth of its frame rate and still read "ok".
- **Leftovers.** No launch while a process of the same binary still runs, and
  the run stops while a program named in `stop_if_running` does.
- **The guard.** Listings (names, sizes, times, never contents) of the `guard`
  folders are compared before and after every row. A folder written
  `folder/**` is listed with everything below it.
- **Stopping.** A file named `PAUSE` in the output folder stops the run before
  the next launch. `--resume` skips the rows that `results.json` already has as
  ok, so a run stopped by power or by `PAUSE` picks up where it left off.
  `--rows A-2,D` runs only those rows or variants.

A profiler capture rides along when a run needs named zones: `{"framelog":
120, "with": {"run": ["~/kisak-build/tracy-capture/tracy-capture", "-o",
"{out}/{row}.tracy", "-a", "127.0.0.1", "-f", "-s", "120"], "name":
"capture"}}`, and the recipe's `after` list runs the export once the game has
quit, so it does not compete with the next launch's load.

### What a run leaves, and how it crosses

    <out>/results.json          everything the report needs, with a checksum
    <out>/results-compact.json  its short form on one line, with its own checksum
    <out>/report.md             the tables and the verbatim lines
    <out>/<row>.log      each launch's console output

Only `results.json` (format `kisak-run-result/1`) has to leave the machine,
and it holds log lines, counts and figures, never game files. It is rewritten
after every row, so a stop loses nothing. A machine that cannot write the
project's shared folder sends it as pasted text, and the receiving side runs

    python3 scripts/runs/run_report.py results.json --check     the checksum
    python3 scripts/runs/run_report.py results.json --out report.md
    python3 scripts/runs/run_report.py results.json --summary   for a message

`run_report.py` finds the JSON inside a pasted message or a code fence. The
checksum is the SHA-256 of the results without their `sha256` entry, written
as JSON with sorted keys, no spaces and ASCII escapes. A file whose lines were
shortened or reworded on the way fails it, and the report then says at its top
that it is not the machine's own words. No table and no verbatim line is
retyped at either end.

When the whole file is too long to relay, the machine pastes
`results-compact.json` instead (`run_report.py results.json --compact` prints
it again): per row the status, the power, the counts that are not zero, what
the scene read, the frame log line as the game printed it and the row's
load note (the three busiest other processes before the launch, at the
capture and during it, and any wait), a few hundred bytes a row. It carries a checksum of its own and the checksum of the file it
was cut from, and `run_report.py` makes the same tables and summary from it.
The other verbatim lines stay in the full file on the machine until someone
asks for them, unless the run says how many of them should cross:
`"compact_lines": 4` in a recipe (or in any results file) keeps up to four
lines per pattern and row in the short form, the first and the last ones with
the number left out between them, each cut at 300 characters;
`run_report.py results.json --compact --lines 4` does the same afterwards. A
run whose point is a handful of log lines (a memory line, a refusal) then
needs no prose beside its short form.

`scripts/tests/test_perf_run.py` checks both scripts on the host with a
stand-in game (`tests/runs/fake_game.cpp`) and a made-up power-supply folder:
the scene steps, every check above, the pause and the resume, and a tampered
results file. The driver has met the real client on no machine yet.

## Short PS Vita starts

A Vita3K or console start should end as soon as it has its numbers:
`vita_quitAfterMenu`, `vita_quitAfterMap`, `vita_statsEvery`, `vita_atMap` and
`vita_assertExit` are described under
[Runs nobody watches](vita.md#runs-nobody-watches).

The hosted `PS Vita multiplayer client` job uploads its RelWithDebInfo ELF with
its symbols beside the `.vpk` (`KisakCOD-mp.elf` in the artifact
`KisakCOD-mp-vita-RelWithDebInfo`), so a crash dump's addresses can be named
with `arm-vita-eabi-addr2line -f -C -i -e KisakCOD-mp.elf <address>` on the
machine that fetched the artifact. The artifact can be downloaded as soon as
that job has finished; the rest of the workflow run need not be done. The job
is in the **PS Vita server and client** workflow (`build-kisakcod-vita.yaml`).

### A Vita3K run from one description: `scripts/runs/vita3k`

On a Windows machine with Vita3K and WSL, `vita-run.ps1 <run.json>` builds a
commit as the CI job does, installs the `.vpk`, runs each start with the
switches above, and writes the same sealed `results.json` and one-line
`results-compact.json` as `perf_run.py`, so `run_report.py` checks and renders
a Vita run like a frame-rate run. A request to that machine is then one JSON
description (commit, starts, the log lines and counts wanted), and its answer
is the short form. The folder's `README.md` has the description's keys, two
examples, how a start ends (Vita3K's "The app exited with code N" line, a
time limit and a kill for what never gets there) and how to set a machine up.
It was written on the project's Windows PC and is checked by hand there:
`scripts/tests/test_vita3k_runner.py` covers only what a Linux host can run
(the sealing script, the shell scripts' syntax, the README's examples).
