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

## Short PS Vita starts

A Vita3K or console start should end as soon as it has its numbers:
`vita_quitAfterMenu`, `vita_quitAfterMap`, `vita_statsEvery`, `vita_atMap` and
`vita_assertExit` are described under
[Runs nobody watches](vita.md#runs-nobody-watches).

The hosted `PS Vita multiplayer client RelWithDebInfo` job uploads the ELF with
its symbols beside the `.vpk` (`KisakCOD-mp.elf` in the artifact
`KisakCOD-mp-vita-RelWithDebInfo`), so a crash dump's addresses can be named
with `arm-vita-eabi-addr2line -f -C -i -e KisakCOD-mp.elf <address>` on the
machine that fetched the artifact. The artifact can be downloaded as soon as
that job has finished; the rest of the workflow run need not be done.
