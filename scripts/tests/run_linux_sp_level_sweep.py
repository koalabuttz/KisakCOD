#!/usr/bin/env python3
"""Opt-in live sweep of singleplayer levels with the native Linux KisakCOD-sp.

Not a CI check: it needs a Call of Duty 4 install and a GPU session, and is run
by hand on a machine that has both. It loads levels one after another, holds
each one for a while, proves the main loop still answers console commands, and
writes a report of what happened. It never decides that a level "works": it
reports load times, errors, assertions and how each process ended, and the
person reading it judges what they see on screen.

Plans
-----
``levels`` (default)
    One process per level: ``+devmap <level>``, wait for the load to finish
    ("Load time: N msec"), hold, ask for ``mapname`` on the console and
    check the answer, then ``quit``.
``chain``
    One process for all levels: ``+devmap`` the first one, then load each next
    one in the same process with ``spdevmap``, which is what the end of a
    mission does (``missionsuccess`` -> ``spmap``/``spdevmap``). This is the
    path that frees one level and loads the next.

``--save-load`` adds, after each hold: ``devsave sweep`` (a console save, the
cheat-protected command devmap allows), a check that the save file appeared
under the profile, ``loadgame sweep`` and a second hold. A death's "retry"
(``loadgame_continue``) goes through the same load.

Isolation
---------
Everything the game writes goes under one work directory:

* ``base/`` is the ``fs_basepath``: symlinks to the install's ``main``,
  ``zone`` and ``localization.txt``. Nothing is written through them (the
  engine writes under ``fs_homepath``; ``fs_copyfiles`` stays 0).
* ``home/`` is the ``fs_homepath``, seeded with a player profile named
  ``sweep`` (``players/profiles/active.txt``), because ``+devmap`` on a profile-less
  home stops with "Tried to use a player profile before it was set".
* ``xdg/`` is ``XDG_DATA_HOME``, and ``cwd/`` the working directory.

Nothing reads, copies or prints game file contents; the report names levels,
files and console lines.

Console input is the process's stdin, which the native build reads as the
dedicated console does. Each process gets its own log ``<level>.log`` (console
output, stdout and stderr together).

``--assert-continue`` sets ``KISAK_ASSERT_CONTINUE=1``: assertions are reported
and the run carries on, so one run lists more of what a level trips over. A
level that passes only in that mode has not passed.

Examples
--------
  python3 scripts/tests/run_linux_sp_level_sweep.py "$GAME" --binary "$BUILD/KisakCOD-sp" \\
      --levels killhouse,cargoship,coup --hold 60 --work-dir ~/kisak-private/sp-sweep1
  python3 scripts/tests/run_linux_sp_level_sweep.py "$GAME" --binary "$BUILD/KisakCOD-sp" \\
      --plan chain --levels killhouse,cargoship --hold 30
  python3 scripts/tests/run_linux_sp_level_sweep.py --list-levels

Exit status: 0 when every step of every level was seen, 1 when any was not,
2 on a usage error.
"""

import argparse
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time

# The stock campaign, in mission order.
STOCK_LEVELS = (
    "killhouse",         # F.N.G.
    "cargoship",         # Crew Expendable
    "coup",              # The Coup
    "blackout",          # Blackout
    "armada",            # Charlie Don't Surf
    "bog_a",             # The Bog
    "hunted",            # Hunted
    "ac130",             # Death from Above
    "bog_b",             # War Pig
    "airlift",           # Shock and Awe
    "aftermath",         # Aftermath
    "village_assault",   # Safehouse
    "scoutsniper",       # All Ghillied Up
    "sniperescape",      # One Shot, One Kill
    "village_defend",    # Heat
    "ambush",            # The Sins of the Father
    "icbm",              # Ultimatum
    "launchfacility_a",  # All In
    "launchfacility_b",  # No Fighting in the War Room
    "jeepride",          # Game Over
    "airplane",          # Mile High Club
)

PROFILE = "sweep"
SAVE_NAME = "sweep"
# The server's map name dvar (sv_init.cpp registers sv_mapname as "mapname").
# The game also registers a read-only "sv_mapname" (g_main.cpp) that singleplayer
# never sets, so asking for that one always answers "".
MAP_DVAR = "mapname"

LOAD_DONE = re.compile(r"^Load time: (\d+) msec")
SERVER_START = re.compile(r"^Server: (\S+)")
# devsave prints "G_WriteGame '<name>' ..." and then "time to write: N ms" when
# the file is written. A level-start or autosave still pending is flushed
# first, with its own "time to write" line, so wait for this save's line.
SAVE_STARTED = re.compile(r"^G_WriteGame '" + re.escape(SAVE_NAME) + "'")
SAVE_WRITTEN = re.compile(r"^time to write: ")
LOADGAME_STARTED = re.compile(r"^=== G_LoadGame ===")
ASSERTION = re.compile(r"ASSERT FAILED: (.*)")
# Com_Error's banner, the start-up failure, and the stubs that abort by name.
ERROR = re.compile(r"^(ERROR: .*|FATAL ERROR: .*|KisakCOD-mp: reached .*|Sys_Error: .*)")
WARNING = re.compile(r"^(WARNING|\^3WARNING)", re.I)
# Under --gdb the exit status is gdb's; the game's comes from gdb's report.
GDB_EXITED = re.compile(r"^\[Inferior \d+ \(process \d+\) exited (normally|with code (\d+))\]")
GDB_SIGNAL = re.compile(r"^(?:Thread \d+ .*|Program) received signal (SIG\w+)")
SIGNALS = {"SIGABRT": 6, "SIGSEGV": 11, "SIGBUS": 7, "SIGFPE": 8, "SIGILL": 4, "SIGKILL": 9, "SIGTERM": 15}


def dvar_answer(name):
    return re.compile(r'^"' + re.escape(name) + r'" is: "([^"]*?)(\^7)?"')


class GameProcess:
    """A KisakCOD-sp process fed through stdin, its output read line by line."""

    def __init__(self, command, cwd, env, log_path):
        self.under_gdb = command[0] == "gdb"
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        self.log.write("$ " + " ".join(shlex.quote(part) for part in command) + "\n")
        self.log.flush()
        self.lines = queue.Queue()
        self.start = time.monotonic()
        self.process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, errors="replace", bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.assertions = []
        self.errors = []
        self.warnings = 0
        self.tail = []
        self.ended = False
        self.gdb_status = None

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def elapsed(self):
        return time.monotonic() - self.start

    def send(self, command):
        self.log.write(f"[sweep +{self.elapsed():.1f}s] > {command}\n")
        self.log.flush()
        try:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
            return True
        except (BrokenPipeError, OSError):
            return False

    def _note(self, line):
        self.log.write(line + "\n")
        self.tail.append(line)
        del self.tail[:-40]
        exited = GDB_EXITED.search(line)
        if exited:
            self.gdb_status = 0 if exited.group(1) == "normally" else int(exited.group(2), 8)
        signalled = GDB_SIGNAL.search(line)
        if signalled and self.gdb_status is None:
            self.gdb_status = -SIGNALS.get(signalled.group(1), 255)
        match = ASSERTION.search(line)
        if match:
            self.assertions.append(match.group(1).strip())
        elif ERROR.search(line):
            self.errors.append(line.strip())
        elif WARNING.search(line):
            self.warnings += 1

    def wait_for(self, pattern, timeout):
        """Reads output until a line matches; returns the match, or None on
        timeout or when the process's output ends."""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                line = self.lines.get(timeout=min(remaining, 0.5))
            except queue.Empty:
                continue
            if line is None:
                self.ended = True
                self.lines.put(None)  # keep the end marker for later readers
                return None
            self._note(line)
            if pattern is not None:
                match = pattern.search(line)
                if match:
                    return match

    def hold(self, seconds):
        """Keeps reading for a while; False when the process ended meanwhile."""
        self.wait_for(None, seconds)
        return self.process.poll() is None

    def finish(self, timeout):
        """Asks the game to quit and returns its exit status (None if it had to
        be killed)."""
        if self.process.poll() is None:
            self.send("quit")
        try:
            deadline = time.monotonic() + timeout
            while self.process.poll() is None and time.monotonic() < deadline:
                self.wait_for(None, 0.5)
            if self.process.poll() is None:
                self.log.write(f"[sweep] no exit {timeout} s after quit; killing\n")
                self.process.kill()
                self.process.wait(10)
                status = None
            else:
                status = self.process.returncode
        finally:
            self.wait_for(None, 1)  # drain what is left
            self.log.close()
            try:
                self.process.stdin.close()
            except OSError:
                pass  # the game is gone; the pipe was already broken
            self.reader.join(5)
            self.process.stdout.close()
        if self.under_gdb and status is not None:
            status = self.gdb_status if self.gdb_status is not None else 255
        return status


def describe_status(status):
    if status is None:
        return "killed after quit timed out"
    if status < 0:
        return f"signal {-status}"
    return f"exit {status}"


def stage(work, game_dir):
    base = work / "base"
    home = work / "home"
    for path in (base, home, work / "xdg", work / "cwd"):
        path.mkdir(parents=True, exist_ok=True)
    for name in ("main", "zone", "localization.txt"):
        link = base / name
        if not link.exists() and (game_dir / name).exists():
            link.symlink_to(game_dir / name)
    profiles = home / "players" / "profiles"
    (profiles / PROFILE).mkdir(parents=True, exist_ok=True)
    active = profiles / "active.txt"
    if not active.exists():
        active.write_text(PROFILE + "\n", encoding="utf-8")
    return base, home


def level_zone_present(game_dir, level):
    zone = game_dir / "zone"
    return any(zone.glob(f"*/{level}.ff")) or (zone / f"{level}.ff").exists()


def game_command(options, base, home, first_level):
    command = [str(options.binary),
               "+set", "fs_basepath", str(base),
               "+set", "fs_homepath", str(home),
               "+set", "fs_copyfiles", "0",
               "+set", "logfile", "2",
               "+set", "r_fullscreen", "0",
               "+set", "r_mode", options.mode,
               "+set", "snd_volume", options.volume]
    for name, value in options.set or []:
        command += ["+set", name, value]
    command += ["+devmap", first_level]
    if options.gdb:
        command = ["gdb", "-q", "-batch", "-ex", "run", "-ex", "bt", "-ex", "thread apply all bt 8",
                   "--args", *command]
    return command


def game_env(options, work):
    env = dict(os.environ)
    env["XDG_DATA_HOME"] = str(work / "xdg")
    if options.assert_continue:
        env["KISAK_ASSERT_CONTINUE"] = "1"
    else:
        env.pop("KISAK_ASSERT_CONTINUE", None)
    return env


class LevelResult:
    def __init__(self, level):
        self.level = level
        self.steps = []          # (name, ok, detail)
        self.load_ms = None
        self.status = "not run"
        self.assertions = []
        self.errors = []
        self.warnings = 0
        self.log = None
        self.tail = []

    def step(self, name, ok, detail=""):
        self.steps.append((name, ok, detail))
        print(f"  {'ok ' if ok else 'BAD'} {self.level}: {name}" + (f" ({detail})" if detail else ""), flush=True)
        return ok

    def ok(self):
        return bool(self.steps) and all(ok for _, ok, _ in self.steps)


def run_level_steps(game, result, options, home, expect_load=True):
    """The steps after a level was asked for. Returns False to stop the process."""
    if expect_load:
        match = game.wait_for(LOAD_DONE, options.load_timeout)
        if match:
            detail = f"{int(match.group(1))} ms"
        elif game.ended:
            detail = "the process ended first"
        else:
            detail = f"no 'Load time' in {options.load_timeout} s"
        if not result.step("loaded", match is not None, detail):
            return False
        result.load_ms = int(match.group(1))
    alive = game.hold(options.hold)
    if not result.step(f"held {options.hold} s", alive, "" if alive else "process ended"):
        return False
    answer = dvar_answer(MAP_DVAR)
    game.send(MAP_DVAR)
    match = game.wait_for(answer, options.probe_timeout)
    if not result.step("console answers", match is not None and match.group(1) == result.level,
                       f"{MAP_DVAR} is '{match.group(1)}'" if match else f"no answer in {options.probe_timeout} s"):
        return False
    if options.save_load:
        save_file = home / "players" / "profiles" / PROFILE / "save" / f"{SAVE_NAME}.svg"
        if save_file.exists():
            save_file.unlink()
        game.send(f"devsave {SAVE_NAME}")
        started = game.wait_for(SAVE_STARTED, options.probe_timeout)
        written = started and game.wait_for(SAVE_WRITTEN, options.probe_timeout)
        if not started:
            detail = f"no \"G_WriteGame '{SAVE_NAME}'\" in {options.probe_timeout} s"
        elif not written:
            detail = f"no 'time to write' in {options.probe_timeout} s"
        elif not save_file.is_file():
            detail = f"{save_file.relative_to(home)} missing"
        else:
            detail = ""
        result.step("devsave written", not detail, detail)
        game.send(f"loadgame {SAVE_NAME}")
        loaded = game.wait_for(LOADGAME_STARTED, options.load_timeout)
        if not result.step("loadgame started", loaded is not None):
            return False
        alive = game.hold(options.hold_after_load)
        if not result.step(f"held {options.hold_after_load} s after loadgame", alive):
            return False
        game.send(MAP_DVAR)
        match = game.wait_for(answer, options.probe_timeout)
        if not result.step("console answers after loadgame", match is not None and match.group(1) == result.level):
            return False
    return True


def collect(game, results):
    for result in results:
        result.assertions = list(dict.fromkeys(game.assertions))
        result.errors = list(dict.fromkeys(game.errors))
        result.warnings = game.warnings
        result.tail = list(game.tail)


def run_levels_plan(options, work, base, home, levels):
    results = []
    for level in levels:
        result = LevelResult(level)
        results.append(result)
        print(f"{level}:", flush=True)
        result.log = work / f"{level}.log"
        game = GameProcess(game_command(options, base, home, level), work / "cwd", game_env(options, work), result.log)
        run_level_steps(game, result, options, home)
        status = game.finish(options.quit_timeout)
        result.status = describe_status(status)
        result.step("clean exit", status == 0, result.status)
        collect(game, [result])
    return results


def run_chain_plan(options, work, base, home, levels):
    results = []
    log = work / ("chain-" + "-".join(levels[:3]) + ("-etc" if len(levels) > 3 else "") + ".log")
    game = GameProcess(game_command(options, base, home, levels[0]), work / "cwd", game_env(options, work), log)
    alive = True
    for index, level in enumerate(levels):
        result = LevelResult(level)
        result.log = log
        results.append(result)
        print(f"{level}:", flush=True)
        if not alive:
            result.step("reached", False, "an earlier level stopped the chain")
            continue
        if index > 0:
            game.send(f"spdevmap {level}")
            started = game.wait_for(SERVER_START, options.load_timeout)
            if not result.step("level change started", started is not None and started.group(1) == level):
                alive = False
                continue
        alive = run_level_steps(game, result, options, home)
    status = game.finish(options.quit_timeout)
    for result in results:
        result.status = describe_status(status)
    results[-1].step("clean exit", status == 0, describe_status(status))
    collect(game, results)
    return results


def write_report(path, options, results, missing):
    lines = ["# Linux singleplayer level sweep", ""]
    lines.append(f"- Binary: `{options.binary}`")
    lines.append(f"- Plan: {options.plan}; hold {options.hold} s; save/load: {'yes' if options.save_load else 'no'}; "
                 f"assertions {'reported and continued (KISAK_ASSERT_CONTINUE=1)' if options.assert_continue else 'fatal'}")
    lines.append(f"- Window {options.mode}, snd_volume {options.volume}"
                 + (", extra: " + ", ".join(f"{n}={v}" for n, v in options.set) if options.set else ""))
    if missing:
        lines.append(f"- Levels without a zone file (not run): {', '.join(missing)}")
    lines += ["", "| Level | Result | Load | Ended | Assertions | Errors | Warnings |",
              "| --- | --- | --- | --- | --- | --- | --- |"]
    for result in results:
        verdict = "all steps seen" if result.ok() else next((name for name, ok, _ in result.steps if not ok), "?")
        if not result.ok():
            verdict = "stopped at: " + verdict
        load = f"{result.load_ms / 1000:.1f} s" if result.load_ms is not None else "-"
        lines.append(f"| {result.level} | {verdict} | {load} | {result.status} | {len(result.assertions)} | "
                     f"{len(result.errors)} | {result.warnings} |")
    for result in results:
        if result.ok() and not result.assertions and not result.errors:
            continue
        lines += ["", f"## {result.level}", "", f"Log: `{result.log}`", ""]
        for name, ok, detail in result.steps:
            lines.append(f"- {'ok' if ok else 'NOT SEEN'}: {name}" + (f" ({detail})" if detail else ""))
        if result.assertions:
            lines += ["", "Assertions:", ""] + [f"- `{text}`" for text in result.assertions[:20]]
        if result.errors:
            lines += ["", "Errors:", ""] + [f"- `{text}`" for text in result.errors[:20]]
        if not result.ok():
            lines += ["", "Last console lines:", "", "```"] + result.tail[-25:] + ["```"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_levels(text):
    if text in (None, "", "all"):
        return list(STOCK_LEVELS)
    levels = [name.strip() for name in text.split(",") if name.strip()]
    for name in levels:
        if not re.fullmatch(r"[A-Za-z0-9_]+", name):
            raise ValueError(f"not a level name: {name!r}")
    return levels


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("game_dir", type=Path, nargs="?", help="Call of Duty 4 installation (read only)")
    parser.add_argument("--binary", type=Path, help="the KisakCOD-sp to run")
    parser.add_argument("--levels", help="comma-separated level names, or 'all' (default: the 21 stock levels)")
    parser.add_argument("--plan", choices=("levels", "chain"), default="levels")
    parser.add_argument("--hold", type=int, default=45, help="seconds to keep each level running (default 45)")
    parser.add_argument("--save-load", action="store_true", help="devsave, then loadgame, in each level")
    parser.add_argument("--hold-after-load", type=int, default=15, help="seconds to hold after loadgame (default 15)")
    parser.add_argument("--load-timeout", type=int, default=240, help="seconds to wait for a load (default 240)")
    parser.add_argument("--probe-timeout", type=int, default=20, help="seconds to wait for a console answer")
    parser.add_argument("--quit-timeout", type=int, default=30, help="seconds to wait for exit after quit")
    parser.add_argument("--mode", default="1280x720", help="r_mode (default 1280x720)")
    parser.add_argument("--volume", default="0.8", help="snd_volume (default 0.8)")
    parser.add_argument("--set", nargs=2, action="append", metavar=("DVAR", "VALUE"), help="extra +set, repeatable")
    parser.add_argument("--assert-continue", action="store_true", help="report assertions and keep going")
    parser.add_argument("--gdb", action="store_true", help="run under gdb -batch and print backtraces on a crash")
    parser.add_argument("--work-dir", type=Path, help="keep logs, home and report here (default: a temporary "
                        "directory that is kept and printed)")
    parser.add_argument("--report", type=Path, help="report path (default: <work-dir>/report.md)")
    parser.add_argument("--list-levels", action="store_true", help="print the stock level list and exit")
    options = parser.parse_args(argv)

    if options.list_levels:
        print("\n".join(STOCK_LEVELS))
        return 0
    if options.game_dir is None or options.binary is None:
        parser.error("the game directory and --binary are required")
    try:
        levels = parse_levels(options.levels)
    except ValueError as error:
        parser.error(str(error))
    if options.hold <= 0 or options.load_timeout <= 0:
        parser.error("--hold and --load-timeout must be positive")
    game_dir = options.game_dir.expanduser().resolve()
    if not (game_dir / "main").is_dir() or not (game_dir / "zone").is_dir():
        parser.error(f"{game_dir} has no main/ and zone/")
    options.binary = options.binary.expanduser().resolve()
    if not options.binary.is_file():
        parser.error(f"{options.binary} does not exist")
    if options.gdb and not shutil.which("gdb"):
        parser.error("--gdb needs gdb on PATH")

    missing = [level for level in levels if not level_zone_present(game_dir, level)]
    levels = [level for level in levels if level not in missing]
    if not levels:
        parser.error("none of the levels has a zone file")

    work = (options.work_dir.expanduser().resolve() if options.work_dir
            else Path(tempfile.mkdtemp(prefix="kisak-sp-sweep-")))
    work.mkdir(parents=True, exist_ok=True)
    base, home = stage(work, game_dir)
    print(f"work directory: {work}", flush=True)
    if missing:
        print(f"no zone file, skipped: {', '.join(missing)}", flush=True)

    plan = run_chain_plan if options.plan == "chain" else run_levels_plan
    results = plan(options, work, base, home, levels)

    report = options.report or work / "report.md"
    write_report(report, options, results, missing)
    passed = sum(result.ok() for result in results)
    print(f"{passed} of {len(results)} levels: every step seen; report: {report}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
