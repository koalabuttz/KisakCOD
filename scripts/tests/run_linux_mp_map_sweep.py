#!/usr/bin/env python3
"""Opt-in live sweep of multiplayer maps with the native Linux KisakCOD-mp.

Not a CI check: it needs a Call of Duty 4 install and a GPU session, and is run
by hand on a machine that has both. It starts each map as a listen server (the
client joins its own server over the in-process loopback, so nothing is
authenticated; the server binds 127.0.0.1 only), joins a team and a class through
the same menu responses the menus send, presses a few buttons, proves the main
loop still answers console commands, and writes a report of what happened. It
never decides that a map "works": it reports load times, errors, assertions,
script errors and how each process ended, and the person reading it judges what
they see on screen.

Plans
-----
``maps`` (default)
    One process per map: ``+devmap <map>``, wait for the client to be in the
    game (``clientinfo`` answers ``state: 9``), join, exercise, hold, check,
    ``quit``.
``chain``
    One process for all maps: ``+devmap`` the first one, then ``devmap`` each
    next one in the same process. This is the path a map rotation takes: it
    frees one map and loads the next.

Steps in each map
-----------------
1. *server started*: the console prints ``Server: <map>``.
2. *in game*: ``clientinfo`` answers ``state: 9``. A first load that drops back
   to the menu is asked for once more and counted in the report.
3. *menus read*: ``sv_serverId`` and the ``configstrings`` listing give the
   script menus the server registered. ``mr <server id> <menu> <response>`` is
   what a menu button sends, and the server ignores it under a stale server id.
4. *joined*: the team menu's ``autoassign``, then the class menu's
   ``assault_mp,0``. The menus are picked by position (``--team-menu`` and
   ``--class-menu``, 8 and 15 as on mp_crash). On later maps they are found by
   the names the first map had at those positions.
5. *exercised*: fire, grenade, run, sprint, jump, reload, switch weapon,
   crouch, all through the console's button commands (``--no-exercise`` skips
   it).
6. *held*: the rest of ``--hold`` seconds.
7. *console answers*: ``mapname`` is the map and the client is still in state 9.
8. *frame drawn* (``--screenshot``): the game's own ``screenshot`` command
   writes a TGA under the work directory; the step fails when the file is
   missing or nearly all one colour. The report describes each image in
   numbers (mean brightness, share that is not black, how many colours, the
   share of the commonest colour, the mean colour of the top, middle and
   bottom third), so maps can be compared without the images leaving the
   machine. ``--delete-screenshots`` removes each image once it is measured;
   otherwise they stay in the work directory for someone to look at.
9. *player was alive*: ``kill`` only acts on a playing client, and the stock
   scripts log the death (a ``K;`` line in ``games_mp.log``). Seeing that line
   shows step 4 really spawned the player.
10. *clean exit*: ``quit`` ends the process with status 0.

Isolation
---------
Everything the game writes goes under one work directory, as in
``run_linux_sp_level_sweep.py``: ``base/`` (the ``fs_basepath``) holds symlinks
to the install's ``main``, ``zone`` and ``localization.txt``; ``home/`` is the
``fs_homepath`` with a player profile named ``sweep``; ``xdg/`` is
``XDG_DATA_HOME``; ``cwd/`` the working directory. The install's own
``players/`` folder is not staged, so no retail profile or stats file is read.

Nothing reads, copies or prints game file contents. The per-map logs hold the
console output, which includes the ``configstrings`` listing (asset names); the
report names maps, menus, files and console lines. Keep the work directory
private.

With ``--sound-debug`` (``snd_alDebug 1``) the report adds the OpenAL driver's
own counts per map: sounds started, sounds cut while playing, stream underruns,
OpenAL errors and the longest gap between sound updates.

Examples
--------
  python3 scripts/tests/run_linux_mp_map_sweep.py "$GAME" --binary "$BUILD/KisakCOD-mp" \\
      --work-dir ~/kisak-private/mp-sweep1 --screenshot --sound-debug
  python3 scripts/tests/run_linux_mp_map_sweep.py "$GAME" --binary "$BUILD/KisakCOD-mp" \\
      --maps mp_crash --hold 1800 --sound-debug          # a long soak of one map
  python3 scripts/tests/run_linux_mp_map_sweep.py "$GAME" --binary "$BUILD/KisakCOD-mp" \\
      --plan chain --maps mp_crash,mp_backlot,mp_shipment --hold 30
  python3 scripts/tests/run_linux_mp_map_sweep.py --list-maps

Exit status: 0 when every step of every map was seen, 1 when any was not,
2 on a usage error.
"""

import argparse
import importlib.util
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The console driver, the staging and the patterns are the singleplayer sweep's.
sp = _load("run_linux_sp_level_sweep", HERE / "run_linux_sp_level_sweep.py")

# The 16 maps the game shipped with, then the five the patches added.
STOCK_MAPS = (
    "mp_backlot",
    "mp_bloc",
    "mp_bog",
    "mp_cargoship",
    "mp_citystreets",
    "mp_convoy",
    "mp_countdown",
    "mp_crash",
    "mp_crossfire",
    "mp_farm",
    "mp_overgrown",
    "mp_pipeline",
    "mp_shipment",
    "mp_showdown",
    "mp_strike",
    "mp_vacant",
    "mp_broadcast",
    "mp_carentan",
    "mp_crash_snow",
    "mp_creek",
    "mp_killhouse",
)

# q_shared.h: the first script menu configstring, and how many there are.
CS_SCRIPT_MENUS = 1970
CS_SCRIPT_MENUS_COUNT = 32
# connstate_t: the client is in the game.
CA_ACTIVE = 9
CA_DISCONNECTED = 0

MAP_DVAR = "mapname"
SERVER_ID_DVAR = "sv_serverId"
TEAM_RESPONSE = "autoassign"
CLASS_RESPONSE = "assault_mp,0"

CLIENT_STATE = re.compile(r"^state: (\d+)")
CONFIGSTRING = re.compile(r"^\s*(\d+): (.*)$")
SCRIPT_ERROR = re.compile(r"\*{7} script (runtime|compile) error \*{7}")
SOUND_SUMMARY = re.compile(r"snd_al: last [\d.]+ s: (\d+) starts, (\d+) cut while playing, (\d+) stream underruns, "
                           r"(\d+) OpenAL errors; .*longest update gap (\d+) ms")
DEATH_LINE = re.compile(r"^\s*\d+:\d+ K;")

# Console button presses: (command, seconds to wait after it). Typed at the
# console, a "+button" stays down until its "-button".
EXERCISE = (
    ("+attack", 2.0), ("-attack", 0.5),
    ("+frag", 0.3), ("-frag", 2.5),
    ("+forward", 2.0),
    ("+breath_sprint", 2.0), ("-breath_sprint", 0.5),
    ("+gostand", 0.2), ("-gostand", 1.0),
    ("-forward", 0.3),
    ("+reload", 0.2), ("-reload", 2.5),
    ("weapnext", 1.5),
    ("+attack", 1.0), ("-attack", 0.5),
    ("+movedown", 0.2), ("-movedown", 1.0),
    ("+moveleft", 1.0), ("-moveleft", 0.2),
    ("+moveright", 1.0), ("-moveright", 0.2),
    ("+gostand", 0.2), ("-gostand", 0.5),
)


def exercise_seconds():
    return sum(delay for _, delay in EXERCISE)


class ClientProcess(sp.GameProcess):
    """The singleplayer sweep's console driver, also counting what only the
    multiplayer client prints and able to collect a listing."""

    def __init__(self, *args):
        self.script_errors = 0
        self.sound = None  # [starts, cuts, underruns, OpenAL errors, longest gap ms]
        self.capture = None
        super().__init__(*args)

    def _note(self, line):
        super()._note(line)
        if self.capture is not None:
            self.capture.append(line)
        if SCRIPT_ERROR.search(line):
            self.script_errors += 1
        summary = SOUND_SUMMARY.search(line)
        if summary:
            values = [int(group) for group in summary.groups()]
            if self.sound is None:
                self.sound = values
            else:
                self.sound = [a + b for a, b in zip(self.sound[:4], values[:4])] + [max(self.sound[4], values[4])]

    def ask(self, command, pattern, timeout):
        self.send(command)
        return self.wait_for(pattern, timeout)

    def client_state(self, timeout):
        match = self.ask("clientinfo", CLIENT_STATE, timeout)
        return int(match.group(1)) if match else None

    def listing(self, command, timeout):
        """Sends a command that prints many lines and returns them. The end is
        marked by the answer to a dvar asked right after it."""
        self.capture = []
        self.send(command)
        done = self.ask(MAP_DVAR, sp.dvar_answer(MAP_DVAR), timeout)
        lines, self.capture = self.capture, None
        return lines if done else None


def menu_ref(text):
    """A menu given on the command line: a position, or a name."""
    return int(text) if text.isdigit() else text


def find_menu(menus, wanted):
    """menus maps position -> name. Returns (position, name) or None."""
    if isinstance(wanted, int):
        return (wanted, menus[wanted]) if wanted in menus else None
    for position, name in sorted(menus.items()):
        if name.lower() == wanted.lower():
            return position, name
    return None


def script_menus(lines):
    menus = {}
    for line in lines:
        match = CONFIGSTRING.match(line)
        if match:
            position = int(match.group(1)) - CS_SCRIPT_MENUS
            if 0 <= position < CS_SCRIPT_MENUS_COUNT:
                menus[position] = match.group(2).strip()
    return menus


# Screenshots are measured the same way in both sweeps.
Frame = sp.Frame


class MapResult(sp.LevelResult):
    def __init__(self, level):
        super().__init__(level)
        self.load_s = None
        self.drops = 0
        self.script_errors = 0
        self.sound = None
        self.menus = ""
        self.frame = None


class Sweep:
    """What carries over from one map to the next: the menu names."""

    def __init__(self, options, home):
        self.options = options
        self.home = home
        self.team_menu = options.team_menu
        self.class_menu = options.class_menu

    def game_log(self):
        return self.home / "main" / "games_mp.log"

    def deaths_logged(self):
        try:
            with open(self.game_log(), encoding="utf-8", errors="replace") as log:
                return sum(1 for line in log if DEATH_LINE.match(line))
        except OSError:
            return None

    def wait_in_game(self, game, result, asked_at):
        """Polls clientinfo until the client is in the game. The server prints
        its "Server:" line after it put the local client into a connecting
        state, so a disconnected client after that line is a load that dropped
        back to the menu. That load is asked for once more."""
        options = self.options
        deadline = time.monotonic() + options.load_timeout
        while time.monotonic() < deadline:
            state = game.client_state(options.probe_timeout)
            if game.ended:
                return "the process ended first"
            if state is None:
                continue  # busy loading: the console is read between frames
            if state == CA_ACTIVE:
                result.load_s = time.monotonic() - asked_at
                return None
            if state == CA_DISCONNECTED:
                if result.drops:
                    return "dropped back to the menu twice"
                result.drops += 1
                game.send(f"devmap {result.level}")
                if not game.wait_for(sp.SERVER_START, max(deadline - time.monotonic(), 1)):
                    return "the process ended first" if game.ended else "no 'Server:' line after the drop"
                continue
            if not game.hold(options.poll):
                return "the process ended first"
        return f"no 'state: {CA_ACTIVE}' in {options.load_timeout} s"

    def join(self, game, result):
        options = self.options
        match = game.ask(SERVER_ID_DVAR, re.compile(sp.dvar_answer(SERVER_ID_DVAR).pattern, re.I),
                         options.probe_timeout)
        lines = game.listing("configstrings", options.probe_timeout) if match else None
        menus = script_menus(lines or [])
        team = find_menu(menus, self.team_menu)
        chosen = find_menu(menus, self.class_menu)
        if not match:
            detail = f"no {SERVER_ID_DVAR} answer in {options.probe_timeout} s"
        elif lines is None:
            detail = "the configstrings listing did not end"
        elif not team or not chosen:
            detail = (f"no menu {self.team_menu!r}" if not team else f"no menu {self.class_menu!r}") \
                     + f" among {len(menus)} script menus"
        else:
            detail = f"server id {match.group(1)}, {team[1]} at {team[0]}, {chosen[1]} at {chosen[0]}"
        if not result.step("menus read", bool(match and team and chosen), detail):
            return False
        result.menus = f"{team[1]} ({team[0]}), {chosen[1]} ({chosen[0]})"
        # Later maps look the menus up by these names, wherever they are.
        self.team_menu, self.class_menu = team[1], chosen[1]
        server_id = match.group(1)
        game.send(f"mr {server_id} {team[0]} {TEAM_RESPONSE}")
        alive = game.hold(options.menu_wait)
        if alive:
            game.send(f"mr {server_id} {chosen[0]} {CLASS_RESPONSE}")
            alive = game.hold(options.menu_wait)
        return result.step("joined", alive, "" if alive else "process ended")

    def run_map(self, game, result, asked_at):
        """The steps after a map was asked for. Returns False to stop the process."""
        options = self.options
        started = game.wait_for(sp.SERVER_START, options.load_timeout)
        if not started:
            detail = "the process ended first" if game.ended else f"no 'Server:' line in {options.load_timeout} s"
        else:
            detail = "" if started.group(1) == result.level else f"started {started.group(1)}"
        if not result.step("server started", bool(started) and not detail, detail):
            return False
        problem = self.wait_in_game(game, result, asked_at)
        detail = problem or f"{result.load_s:.1f} s" + (", after one drop to the menu" if result.drops else "")
        if not result.step("in game", problem is None, detail):
            return False
        if not self.join(game, result):
            return False
        held_from = time.monotonic()
        if not options.no_exercise:
            alive = True
            for command, delay in EXERCISE:
                game.send(command)
                alive = game.hold(delay)
                if not alive:
                    break
            if not result.step("exercised", alive, "" if alive else "process ended"):
                return False
        remaining = max(options.hold - (time.monotonic() - held_from), 1)
        alive = game.hold(remaining)
        if not result.step(f"held {options.hold} s", alive, "" if alive else "process ended"):
            return False
        match = game.ask(MAP_DVAR, sp.dvar_answer(MAP_DVAR), options.probe_timeout)
        state = game.client_state(options.probe_timeout) if match else None
        if not match:
            detail = f"no answer in {options.probe_timeout} s"
        else:
            detail = f"{MAP_DVAR} is '{match.group(1)}', client state {state}"
        if not result.step("console answers", bool(match) and match.group(1) == result.level and state == CA_ACTIVE,
                           detail):
            return False
        if (options.screenshot and not sp.take_screenshot(game, result, self.home, options, f"sweep_{result.level}")
                and game.ended):
            return False
        before = self.deaths_logged()
        game.send("kill")
        deadline = time.monotonic() + options.probe_timeout
        after = before
        while time.monotonic() < deadline and game.hold(0.5):
            after = self.deaths_logged()
            if after is not None and after > (before or 0):
                break
        if after is None:
            detail = f"no {self.game_log().relative_to(self.home)}"
        elif after > (before or 0):
            detail = ""
        else:
            detail = f"no death logged {options.probe_timeout} s after kill"
        result.step("player was alive", not detail, detail)
        return not game.ended


def game_command(options, base, home, first_map):
    command = [str(options.binary),
               "+set", "fs_basepath", str(base),
               "+set", "fs_homepath", str(home),
               "+set", "fs_copyfiles", "0",
               # The listen server binds the loopback address only.
               "+set", "net_ip", "127.0.0.1",
               "+set", "logfile", "2",
               "+set", "g_log", "games_mp.log",
               "+set", "g_logSync", "1",
               "+set", "r_fullscreen", "0",
               "+set", "r_mode", options.mode,
               # The default position (3, 22) can push the window past the
               # edge of a scaled screen, and screenshot then refuses a window
               # it sees as partly off screen. A --set can still move it.
               "+set", "vid_xpos", "0",
               "+set", "vid_ypos", "0",
               "+set", "snd_volume", options.volume]
    if options.sound_debug:
        command += ["+set", "snd_alDebug", "1"]
    for name, value in options.set or []:
        command += ["+set", name, value]
    command += ["+devmap", first_map]
    if options.gdb:
        command = ["gdb", "-q", "-batch", "-ex", "run", "-ex", "bt", "-ex", "thread apply all bt 8",
                   "--args", *command]
    return command


def collect(game, results):
    sp.collect(game, results)
    for result in results:
        result.script_errors = game.script_errors
        result.sound = game.sound


def run_maps_plan(options, work, base, home, maps):
    sweep = Sweep(options, home)
    results = []
    for name in maps:
        result = MapResult(name)
        results.append(result)
        print(f"{name}:", flush=True)
        result.log = work / f"{name}.log"
        asked_at = time.monotonic()
        game = ClientProcess(game_command(options, base, home, name), work / "cwd", sp.game_env(options, work),
                             result.log)
        sweep.run_map(game, result, asked_at)
        status = game.finish(options.quit_timeout)
        result.status = sp.describe_status(status)
        result.step("clean exit", status == 0, result.status)
        collect(game, [result])
    return results


def run_chain_plan(options, work, base, home, maps):
    sweep = Sweep(options, home)
    results = []
    log = work / ("chain-" + "-".join(maps[:3]) + ("-etc" if len(maps) > 3 else "") + ".log")
    asked_at = time.monotonic()
    game = ClientProcess(game_command(options, base, home, maps[0]), work / "cwd", sp.game_env(options, work), log)
    alive = True
    for index, name in enumerate(maps):
        result = MapResult(name)
        result.log = log
        results.append(result)
        print(f"{name}:", flush=True)
        if not alive:
            result.step("reached", False, "an earlier map stopped the chain")
            continue
        if index > 0:
            asked_at = time.monotonic()
            game.send(f"devmap {name}")
        alive = sweep.run_map(game, result, asked_at)
    status = game.finish(options.quit_timeout)
    for result in results:
        result.status = sp.describe_status(status)
    results[-1].step("clean exit", status == 0, sp.describe_status(status))
    collect(game, results)
    return results


def write_report(path, options, results, missing):
    chain = options.plan == "chain"
    lines = ["# Linux multiplayer map sweep", ""]
    lines.append(f"- Binary: `{options.binary}`")
    lines.append(f"- Plan: {options.plan}; hold {options.hold} s; exercise: {'no' if options.no_exercise else 'yes'}; "
                 f"screenshot: {'yes' if options.screenshot else 'no'}; "
                 f"assertions {'reported and continued (KISAK_ASSERT_CONTINUE=1)' if options.assert_continue else 'fatal'}")
    lines.append(f"- Window {options.mode}, snd_volume {options.volume}"
                 + (", extra: " + ", ".join(f"{n}={v}" for n, v in options.set) if options.set else ""))
    if missing:
        lines.append(f"- Maps without a zone file (not run): {', '.join(missing)}")
    if chain:
        lines.append("- One process ran every map, so the assertion, error, script error and sound counts are the "
                     "whole chain's on every row.")
    lines += ["", "| Map | Result | In game after | Drops | Frame | Ended | Assertions | Errors | Script errors | Warnings |",
              "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for result in results:
        verdict = "all steps seen" if result.ok() else next((name for name, ok, _ in result.steps if not ok), "?")
        if not result.ok():
            verdict = "stopped at: " + verdict
        load = f"{result.load_s:.1f} s" if result.load_s is not None else "-"
        frame = f"{result.frame.mean:.0f}" if result.frame else "-"
        lines.append(f"| {result.level} | {verdict} | {load} | {result.drops} | {frame} | "
                     f"{result.status} | {len(result.assertions)} | {len(result.errors)} | {result.script_errors} | "
                     f"{result.warnings} |")
    if any(result.sound for result in results):
        lines += ["", "Sound (the OpenAL driver's own counts, `snd_alDebug 1`):", "",
                  "| Map | Starts | Cut while playing | Stream underruns | OpenAL errors | Longest update gap |",
                  "| --- | --- | --- | --- | --- | --- |"]
        for result in results:
            if result.sound:
                starts, cuts, underruns, al_errors, gap = result.sound
                lines.append(f"| {result.level} | {starts} | {cuts} | {underruns} | {al_errors} | {gap} ms |")
    lines += sp.frame_table(results, "map")
    menus = list(dict.fromkeys(result.menus for result in results if result.menus))
    if menus:
        lines += ["", "Menus answered (name and position): " + "; ".join(menus) + "."]
    for result in results:
        if result.ok() and not result.assertions and not result.errors and not result.script_errors:
            continue
        lines += ["", f"## {result.level}", "", f"Log: `{result.log}`", ""]
        for name, ok, detail in result.steps:
            lines.append(f"- {'ok' if ok else 'NOT SEEN'}: {name}" + (f" ({detail})" if detail else ""))
        if result.assertions:
            lines += ["", "Assertions:", ""] + [f"- `{text}`" for text in result.assertions[:20]]
        if result.errors:
            lines += ["", "Errors:", ""] + [f"- `{text}`" for text in result.errors[:20]]
        if result.script_errors:
            lines += ["", f"Script errors: {result.script_errors} (search the log for \"script runtime error\")."]
        if not result.ok():
            lines += ["", "Last console lines:", "", "```"] + result.tail[-25:] + ["```"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_maps(text):
    if text in (None, "", "all"):
        return list(STOCK_MAPS)
    maps = [name.strip() for name in text.split(",") if name.strip()]
    for name in maps:
        if not re.fullmatch(r"[A-Za-z0-9_]+", name):
            raise ValueError(f"not a map name: {name!r}")
    return maps


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("game_dir", type=Path, nargs="?", help="Call of Duty 4 installation (read only)")
    parser.add_argument("--binary", type=Path, help="the KisakCOD-mp to run")
    parser.add_argument("--maps", help="comma-separated map names, or 'all' (default: the 21 stock maps)")
    parser.add_argument("--plan", choices=("maps", "chain"), default="maps")
    parser.add_argument("--hold", type=int, default=45,
                        help="seconds to keep each map running after joining, the exercise included (default 45)")
    parser.add_argument("--no-exercise", action="store_true", help="join and hold, without pressing buttons")
    parser.add_argument("--screenshot", action="store_true",
                        help="take one screenshot per map into the work directory, describe it in numbers and "
                             "check it is not all one colour")
    parser.add_argument("--delete-screenshots", action="store_true",
                        help="with --screenshot: delete each image once its numbers are taken")
    parser.add_argument("--sound-debug", action="store_true",
                        help="set snd_alDebug 1 and report the OpenAL driver's counts")
    parser.add_argument("--team-menu", type=menu_ref, default=8, metavar="MENU",
                        help="the team menu: its position among the script menus, or its name (default 8)")
    parser.add_argument("--class-menu", type=menu_ref, default=15, metavar="MENU",
                        help="the menu that takes the class: position or name (default 15)")
    parser.add_argument("--menu-wait", type=float, default=3, help="seconds to wait after each menu response")
    parser.add_argument("--poll", type=float, default=2, help="seconds between clientinfo polls while loading")
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
    parser.add_argument("--list-maps", action="store_true", help="print the stock map list and exit")
    options = parser.parse_args(argv)

    if options.list_maps:
        print("\n".join(STOCK_MAPS))
        return 0
    if options.game_dir is None or options.binary is None:
        parser.error("the game directory and --binary are required")
    try:
        maps = parse_maps(options.maps)
    except ValueError as error:
        parser.error(str(error))
    if options.hold <= 0 or options.load_timeout <= 0:
        parser.error("--hold and --load-timeout must be positive")
    if not options.no_exercise and options.hold < exercise_seconds():
        parser.error(f"--hold must be at least {exercise_seconds():.0f} s, the length of the exercise "
                     "(or pass --no-exercise)")
    game_dir = options.game_dir.expanduser().resolve()
    if not (game_dir / "main").is_dir() or not (game_dir / "zone").is_dir():
        parser.error(f"{game_dir} has no main/ and zone/")
    options.binary = options.binary.expanduser().resolve()
    if not options.binary.is_file():
        parser.error(f"{options.binary} does not exist")
    if options.gdb and not shutil.which("gdb"):
        parser.error("--gdb needs gdb on PATH")

    missing = [name for name in maps if not sp.level_zone_present(game_dir, name)]
    maps = [name for name in maps if name not in missing]
    if not maps:
        parser.error("none of the maps has a zone file")

    work = (options.work_dir.expanduser().resolve() if options.work_dir
            else Path(tempfile.mkdtemp(prefix="kisak-mp-sweep-")))
    work.mkdir(parents=True, exist_ok=True)
    base, home = sp.stage(work, game_dir)
    print(f"work directory: {work}", flush=True)
    if missing:
        print(f"no zone file, skipped: {', '.join(missing)}", flush=True)

    plan = run_chain_plan if options.plan == "chain" else run_maps_plan
    results = plan(options, work, base, home, maps)

    report = options.report or work / "report.md"
    write_report(report, options, results, missing)
    passed = sum(result.ok() for result in results)
    print(f"{passed} of {len(results)} maps: every step seen; report: {report}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
