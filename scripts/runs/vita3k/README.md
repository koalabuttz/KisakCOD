# vita-run.ps1: one PS Vita client run in Vita3K from one description

One JSON description in, one results file out, one command per run:

    pwsh -File vita-run.ps1 <run.json>           run it
    pwsh -File vita-run.ps1 <run.json> -Check    read the description and print what would be run

It builds the commit in WSL (`wsl/03-build.sh`, the steps of the CI job "PS Vita multiplayer client
RelWithDebInfo"), installs the `.vpk` into Vita3K, runs each start, and writes `runs/<id>/results.json` in the
repository's `kisak-run-result/1` format (docs/runs.md), sealed with `run_report.py`'s checksum, plus
`runs/<id>/results-compact.json`, the one-line short form that is pasted into a report. As its last step it
runs `run_report.py results.json --check` and prints the result and the compact line.

Results are local Vita3K results from a local build, not CI evidence. The results hold the engine's own log
lines, counts and figures. No game file content goes into them and no image is kept.

Written on the Windows PC that runs the project's Vita3K starts (2026-11-10) and kept here so that the
next machine does not start from nothing. What has run through it there: menu and map starts with timed
commands, a view step, marker files, key input, a stop pattern, builds of a commit. Not yet used there:
`merge`, `cmake`, `cold_cache`, typing into a Vita3K dialog, and the fallback for builds without the run
switches. PowerShell cannot run in the project's CI, so the hosted check
(`scripts/tests/test_vita3k_runner.py`) covers the sealing script, the shell scripts' syntax and the examples
below, not the runner itself.

## Setting a machine up

The runner expects a work folder with Vita3K in `<work>\app\` and its own files in `<work>\tools\`:
copy this folder's files there (keeping `wsl\`), and `scripts/runs/run_report.py` beside them. It needs
PowerShell 7 (`pwsh`), Python 3, and WSL with a distribution named `Ubuntu`. In WSL, once:
`wsl/01-look-and-clone.sh [branch]` clones the repository to `~/kisak-vita`, and `wsl/02-install.sh`
installs the packages, the pinned VitaSDK and the pinned DXVK Native headers as the CI job does and writes
`~/kisak-build/env.sh`, which `wsl/03-build.sh` reads. Both install software: on a machine that is not yours,
ask its owner first. The app's title id is `KSKD00002` and the emulated card is Vita3K's default
(`%APPDATA%\Vita3K\Vita3K\ux0`). A file `machine.txt` beside the runner, one line, is what the results
call the machine (default: the computer's name); it stays on the machine.

## Files

    vita-run.ps1        the runner
    vita_seal.py        seals, checks and shortens the results with run_report.py
    run_report.py       a copy of scripts/runs/run_report.py from the repository
    wsl/03-build.sh     the build (fetch, detached checkout, optional local merge, build, binary checks)
    tap.ps1             a key or a click for the Vita3K window (clicks are front touches, arrows the d-pad)
    shot.ps1            one capture of the Vita3K window
    shotstats.ps1       mean brightness and share of non-black pixels of a capture
    wsl/01-look-and-clone.sh, wsl/02-install.sh    the one-time set-up in WSL

## The description

    {
      "id": "run-x",                     names runs/<id>/; default: the file's name
      "title": "...", "asker": "...",    copied into the results
      "commit": "<sha or branch>",       what to build; or "use_installed": true to run what is installed
      "merge": "<sha>",                  optional: merged locally with plain git merge --no-edit; never pushed;
                                         a conflict stops the run and is reported
      "cmake": ["-DKISAK_VITA_HEAP_MB=128"],   optional extra configure options; gives the build its own
                                         build folder
      "name": "run-x-build",             the build's folder under local-builds/; default: the commit's full
                                         SHA, or <commit>+<merge>, or <commit>-opts
      "cold_cache": false,               true: the cached programs are moved aside to gxp-before-<id> (not
                                         deleted) before the install; otherwise the cache is never touched
      "options": {"no_assert_exit": false, "no_safety_net": false},
      "starts": [ { ...one object per start... } ]
    }

A start:

    "tag": "menu",                       names the row and its files
    "cmdline": "+set snd_volume 0.8",    the command line as given
    "markers": ["no-target-reserve"],    empty files created in the card's gxp folder before the start and
                                         removed after it (one that was already there is left there)
    "stop": {"seconds": 300,             the hard limit; the window is closed 45 s after it
             "pattern": "regex", "after": 10},   optional: close the window this long after a log line
    "lines": ["Vita memory", "^.*frame log"],    regexes (case-sensitive); every matching log line is returned
                                         verbatim, in log order, under its pattern
    "counts": ["WARNING"],               regexes; the number of matching lines, under the pattern
    "input": [ ...timed steps... ],      optional, for menus
    "quit_after_menu": 15, "quit_after_map": 30, "stats_every": 10,
    "at_menu": "...", "at_map": "3 cmd mr {serverid} 8 autoassign | 8 cmd mr {serverid} 15 assault_mp,0"

An input step is a trigger and an action. Trigger: `"at": <seconds after the start>`, or `"after": "<regex>"`
with `"plus": <seconds>` (that long after the first log line matching it). Action: `"key": "Down"` (X, Enter,
Up, Down, Left, Right, or a letter; `"repeat"`, `"hold_ms"`), `"click": [x, y]` (window-relative, `"hold_ms"`),
or `"view": "label"` (takes a capture, writes its numbers into the row, deletes the image). Steps run in order.

What the runner adds to a command line:

- `+set vita_assertExit 1`, unless `no_assert_exit` or the command line already names it.
- `+set vita_quitAfter <the hard limit>` when the command line has no `vita_quitAfter`, unless `no_safety_net`.
- The switches for unattended runs, when the build has them (the runner looks for their names in the kept
  ELF): `quit_after_menu` becomes `+set vita_quitAfterMenu`, `quit_after_map` `+set vita_quitAfterMap`,
  `stats_every` `+set vita_statsEvery`, `at_menu` and `at_map` `+set vita_atMenu "..."` / `vita_atMap "..."`.
  In a build without them the runner falls back to a fixed length (`vita_quitAfter` = the asked seconds plus
  45 for the menu or 90 for a map), does not send the timed commands, and says so in the row's `note`. Use
  `input` steps for the team and class menus there.

## Two examples

A menu start:

    {
      "id": "example-menu",
      "commit": "claude/project-thread-onteee",
      "starts": [
        {"tag": "menu", "cmdline": "+set snd_volume 0.8", "quit_after_menu": 15, "stats_every": 10,
         "stop": {"seconds": 120}, "lines": ["Vita memory", "gpu memory by use"], "counts": ["WARNING"]}
      ]
    }

A map start on two commits merged locally, with the picks sent by the game itself:

    {
      "id": "example-map",
      "commit": "3b96fde2", "merge": "d365a52a", "name": "3b96fde2+d365a52a",
      "starts": [
        {"tag": "map", "cmdline": "+set snd_volume 0.8 +devmap mp_crash",
         "quit_after_map": 30, "stats_every": 10,
         "at_map": "3 cmd mr {serverid} 8 autoassign | 8 cmd mr {serverid} 15 assault_mp,0 | 16 framelog 8 map",
         "stop": {"seconds": 420},
         "input": [{"after": "Vita: the map is up", "plus": 14, "view": "after the class pick"}],
         "lines": ["Vita memory", "draws( skipped)?:", "sound data:"],
         "counts": ["out of GPU memory for a resource \\(render targets"]}
      ]
    }

On a build without the switches the same start needs clicks instead of `at_map` (positions of the default
976x583 window): `{"after": "CL_InitCGame: ", "plus": 25, "click": [322, 330], "hold_ms": 1200}` for
Auto-Assign and `{"after": "CL_InitCGame: ", "plus": 0, "click": [337, 110], "hold_ms": 1500}` 14 s later for
Assault (give the second step `"at"` or its own pattern; steps run in order).

## The results

`runs/<id>/results.json`, one JSON object:

    "format": "kisak-run-result/1", "id", "title", "asker", "machine", "started", "finished" (UTC), "status"
    "build": asked, commit, name, merge_commit, cmake, seconds, compiler, binary_checks, elf_sha256,
             eboot_sha256, unattended_switches (all strings)
    "rows": one per start:
        "name", "variant" (the tag), "status" ("ok" or "failed: why"),
        "exit" (the app's exit code as Vita3K reports it; null when the app never left),
        "seconds" (from the launch to the app's exit, or to the moment the script ended the start)
        "values": {"ended": "by itself" | "by itself; Vita3K stayed after the app had left and was
                             closed|killed by the script" | "closed by the script after its stop pattern"
                             (", then killed (the window did not close)") | "closed|killed by the script
                             at the hard limit",
                   "log": "whole" (ends with the shutdown line) | "the file as written, without the shutdown
                           line" (an exit on an error; nothing had to be added) | "tail from the echo"}
        "lines": {"<pattern>": [verbatim log lines]}
        "counts": asserts, errors, warnings, out_of_gpu_memory, no_memory, vita3k_invalid_access,
                  vita3k_exceptions, and one entry per pattern of the description
        "framelog": [{"label", "line"}] for every "frame log ...: ... fps" line
        "outputs": "command line as run", "where the lines come from", the views' numbers, and for every
                   faulting address in Vita3K's log the function and line from addr2line with the kept ELF
        "note": what fell back or was left alone
        "tail": the last 30 log lines, when the row is not ok
    "state": installed_build, installed_eboot_sha256, cmdline_mp_txt, programs_in_gxp, marker_files
    "sha256": run_report.py's checksum (the SHA-256 of the object without this key, as JSON with sorted keys,
              separators "," and ":", ASCII escapes)

A row is ok when the app left by itself with exit code 0, or was closed after the stop pattern the description
asked for, and no assertion failed. Reaching the hard limit, another exit code or a failed assertion make it
"failed: ...". A start that is meant to end with an error (a test of the error path) therefore reads
"failed: exit code 1"; say in the report that this was the wanted ending.

How a start ends. The runner reads Vita3K's own log while the app runs. When Vita3K writes "The app exited
with code N" the start is over: N is the row's exit code, and if Vita3K's process is still there 8 s later
(it stays for ever after an exit code other than 0) the runner closes it, and kills it when its window does
not close within 20 s. A fault or abort() in the app never gets that far: Vita3K hangs, and the start ends at
the hard limit (the "stop" seconds plus 45).

The log's tail. Vita3K keeps the app's file writes in a 4096-byte buffer of its own until the file is closed,
and it does not implement sceIoSyncByFd. So whenever Vita3K has to be killed, the log file stops at a multiple
of 4096 bytes. The runner then takes the lines after the file's last whole line from Vita3K's echo of the
app's standard output, with times worked out from Vita3K's clock, and says so in "where the lines come from".
Lines the game writes only to its log file are not in the echo.

Beside the results the run folder holds, per start, Vita3K's own log (`<tag>.out`), the app's log
(`<tag>-console-mp.log`), the description as run and the build log. They stay on the machine.

## Rules it keeps

- It copies nothing off the machine. What crosses is the compact line, pasted by whoever reports the run.
- It writes under the Vita3K work folder (runs/, local-builds/, installed-build.txt); on the emulated card
  only cmdline-mp.txt and the marker files it was asked for; in WSL only through 03-build.sh.
- It removes the marker files it created.
- It never empties the program cache unless the description says "cold_cache": true, and then it moves the
  programs aside.
- Local merges are never pushed.
- It refuses to start while Vita3K is running.
