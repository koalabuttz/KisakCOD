#!/usr/bin/env python3
"""Run a picture or frame-rate run of the game from one recipe, unattended.

Not a CI check: it needs the game and its data, and runs on a test machine
(docs/runs.md, "A Windows run from one recipe: pc_run.py"). Written for the
Windows PC, where every launch used to be driven by hand: typing into the
console, holding the window's focus, timing each step by eye, and writing a
new script for every set of picture numbers. Standard library only, so it runs
with the Python the PC already has; it also runs on Linux.

Each launch plays its own scene with the client's timed console commands
(``run_atMap``, docs/runs.md "Runs that play themselves"): the recipe lists
them per launch, and the game takes its screenshots by name
(``screenshot <name>`` writes ``screenshots/<name>.tga``) and quits by itself.
The runner then collects, per launch: the exit status, the wanted log lines
verbatim, the frame log lines, and the screenshots; and over the screenshots
the numbers the steps files keep asking for: mean colour per region, and for
each comparison the mean difference, the share of pixels that differ by more
than N levels, where those pixels are (boxes), a side-by-side picture, a
difference picture (x4) and a full-size crop around the biggest change.

    pc_run.py recipe.json --out <folder>          run it
    pc_run.py recipe.json --out <folder> --dry-run   print the launches only
    pc_run.py recipe.json --out <folder> --resume    skip launches already done
    pc_run.py --pictures-only recipe.json --out <folder>   redo the numbers

It writes ``<out>/results.json`` (everything, checksummed like perf_run.py's,
see run_report.py), ``<out>/report.md``, ``<out>/<launch>/`` with the log and
the screenshots, and ``<out>/pictures/`` with the PNGs. Game pictures and logs
stay on the machine; the report and results.json are what crosses.

The recipe (JSON)::

    {
      "format": "kisak-pc-run/1",
      "id": "pc-ao-run-10",
      "title": "AO edge, one launch per view",
      "game": {
        "exe": "C:/kisak/builds/b204c3a-vk/KisakCOD-mp.exe",
        "cwd": "C:/kisak/builds/b204c3a-vk",
        "args": ["+set", "r_fullscreen", "1", "+set", "r_mode", "1280x720", "+set", "r_aaSamples", "1",
                 "+set", "com_maxfps", "0", "+set", "snd_volume", "0", "+set", "logfile", "2"],
        "env": {"KISAK_VK_MODERN_EXPOSURE": "0"},
        "clear_env": ["KISAK_VK_*"],
        "home_template": "C:/kisak/homes/template",
        "timeout": 300,
        "forbid_processes": ["steam.exe"]
      },
      "launches": [
        {"name": "crash-in",
         "args": ["+set", "r_backend", "vulkan", "+set", "r_visuals", "modern", "+devmap", "mp_crash"],
         "env": {"KISAK_VK_MODERN_DEBUG": "ao"},
         "at_menu": [],
         "at_map": ["3 cmd mr {serverid} 8 autoassign", "5 cmd mr {serverid} 15 assault_mp,0",
                    "8 setviewpos 30 -10 200 140",
                    "10 framelog 10 crash-in", "21 screenshot crash-in-D", "23 quit"]}
      ],
      "lines": ["modern:", "device destroyed after", "refused", "unsupported", "could not", "failed"],
      "pictures": {
        "regions": {"arch": [455, 300, 505, 400]},
        "levels": [2, 16],
        "compare": [{"a": "crash-in-B", "b": "crash-in-D", "label": "B vs D"}]
      }
    }

``game.home_template`` (optional) is copied to ``<out>/<launch>/home`` for
every launch and given as ``fs_homepath``, so each launch starts from the
same home folder and its screenshots and log are its own. Without it the
runner looks for new screenshots under ``game.cwd``. ``at_map`` gets a safety
``run_quitAfterMap`` (the last command's time plus 30 s) unless the launch
gives its own; ``timeout`` (seconds, whole launch) ends a launch that hangs.
Every launch gets ``KISAK_NO_DIALOGS=1`` unless the recipe sets it, so the
game answers its own questions ("Set Optimal Settings?") "No" instead of
waiting. Pin the settings a run depends on in ``game.args`` (``r_aaSamples 1``
for anything Modern: a multisampled back buffer turns the Modern path off;
``com_maxfps 0`` for anything timed, or the frame log reads the cap),
since the home template's profile holds the player's own.
``forbid_processes`` are checked before and after every launch (Windows:
tasklist); a launch never starts while one runs, nor while another copy of
the game does.

Settings compared inside one launch: ``variants``. The Modern switches are
live dvars (``r_modernAo``, ``r_modernSsr``, ... ; docs/runs.md), so one launch
can show every value at the same camera, and its frame rates are not spread by
the drift between launches. A launch lists its variants and the runner writes
the timed commands::

    {"name": "crash-in", "args": ["+devmap", "mp_crash"],
     "at_map": ["3 cmd mr {serverid} 8 autoassign", "5 cmd mr {serverid} 15 assault_mp,0",
                "8 setviewpos 30 -10 200 140"],
     "variants": [{"name": "B", "set": {"r_modernAo": "off"}},
                  {"name": "A", "set": {"r_modernAo": "on"}}],
     "variant_timing": {"start": 20, "framelog": 8}}

From ``start`` seconds after the map is up (default: 20 s, after the
pre-match countdown, whose grey picture and missing HUD would swamp any
difference; or 2 s after the last ``at_map`` command if that is later) each variant in turn sets its dvars, waits ``settle``
seconds (default 1: AO was fully in 0.1 s after a switch; the first
variant also waits ``warmup`` seconds, default 30 when frame logs are taken,
because a launch's first half minute in play ran 6-11% slower), takes the screenshot
``<launch>-<variant>`` the first time round and, when ``framelog`` is given,
a frame log ``<launch>-<variant>-<pass>`` of that many seconds; then the
launch quits. With a frame log the variants run twice, in mirrored order
(A B B A), so steady drift cancels in the mean; ``"passes": 1`` or ``2`` sets
it. ``"compare": true`` compares the first variant's screenshot with each
other one's. Per launch, results.json gets ``variants`` (fps per pass, mean
fps, and the difference from the first variant) and the report a table. Each
variant's ``set`` should name every dvar any variant changes, so the order of
the passes does not matter. Moving things (smoke, the helicopter) move
between the screenshots; ``"18 timescale 0.001"`` in ``at_map`` (devmap,
cheats on) holds them still (variants proof 1: the same variant twice differed
by 0.29 levels), so use it for picture launches but not for frame logs.
AO looked lighter frozen than in play (arch 17.3 against 22.1 levels at the
same radius and view, proof 2), so compare frozen shots with frozen shots.

The game splits its command line at every ``+`` (Com_ParseCommandLine) and
keeps 31 parts, so a timed command or a variant value with a ``+`` in it
(``45 +left``) would break the whole list: the recipe check refuses it. Put
such commands in a cfg in the home template and send ``exec <cfg>``. A launch
whose timed list the game refused anyway ends with status ``run_error``.
The game also keeps only 1023 bytes of its command line (``sys_cmdline``):
on Windows it cuts the rest, timed list and all, so the recipe check refuses
a launch whose command line would be longer (a long variant list is the
usual cause; split it over two launches).

Exit status: 0 when every launch exited 0, ran its timed commands and wrote
every screenshot it named, 1 otherwise (the report says which), 2 on a bad
recipe.
"""

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

FORMAT = "kisak-pc-run/1"
# Windows copies its command line into sys_cmdline[1024] and cuts the rest
# (src/win32/win_main.cpp); Linux and Android refuse to start (src/linux/main.cpp).
COMMAND_LINE_BYTES = 1023
MAX_CONSOLE_LINES = 31  # Com_ParseCommandLine keeps 32 lines, the first is what comes before the first '+'
SAFETY_AFTER_LAST = 30
# Variants start after the pre-match countdown (about 16-17 s on mp_crash under
# devmap: a grey vision set and no HUD, pc_run variants proof 1).
DEFAULT_VARIANT_START = 20
SPREAD_WARNING_PCT = 5
# Variants proof 2: AO was fully in 0.1 s after the switch, so 1 s settles.
DEFAULT_SETTLE = 1
# Both proofs: the first frame log of a launch ran 6-11% slower than the same
# variant 40 s later, while later passes agreed within 1%; a launch with frame
# logs waits this long on its first variant before it starts measuring.
DEFAULT_WARMUP = 30  # seconds after the last timed command before the safety quit
FRAME_LOG = re.compile(r"^frame log (\S+): .*$")
FRAME_LOG_FPS = re.compile(r"^frame log (\S+): (\d+) frames in ([\d.]+) s: ([\d.]+) fps; ms mean ([\d.]+), "
                           r"median ([\d.]+), p95 ([\d.]+), p99 ([\d.]+)")
DVAR_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
SHOT_COMMAND = re.compile(r"^\s*[\d.]+\s+screenshot\s+(\S+)\s*$")
TIMED = re.compile(r"^\s*([\d.]+)\s+\S")


class RecipeError(Exception):
    pass


# ---------------------------------------------------------------- the recipe

def load_recipe(path):
    try:
        recipe = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RecipeError(f"cannot read {path}: {error}")
    if recipe.get("format") != FORMAT:
        raise RecipeError(f"format is {recipe.get('format')!r}, expected {FORMAT!r}")
    game = recipe.get("game") or {}
    if not game.get("exe"):
        raise RecipeError("game.exe is missing")
    launches = recipe.get("launches") or []
    if not launches:
        raise RecipeError("no launches")
    names, shots = set(), set()
    for launch in launches:
        name = launch.get("name", "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name or ""):
            raise RecipeError(f"launch name {name!r}: letters, digits, _ . - only")
        if name in names:
            raise RecipeError(f"launch name {name!r} given twice")
        names.add(name)
        check_variants(launch)
        for key in ("at_menu", "at_map"):
            commands = launch_commands(launch, key)
            if len(commands) > 32:
                raise RecipeError(f"{name}: {key} has {len(commands)} commands; the client takes 32")
            for command in commands:
                if "|" in command or not TIMED.match(command):
                    raise RecipeError(f"{name}: {key} entry {command!r} is not '<seconds> <command>' without '|'")
                if "+" in command or "\n" in command:
                    raise RecipeError(f"{name}: {key} entry {command!r} has a '+': the game splits its command line "
                                      "at every '+', so the whole list would be refused; put '+left' and the like "
                                      "in a cfg and send 'exec <cfg>'")
                if len(command.split(None, 1)[1]) >= 160:
                    raise RecipeError(f"{name}: {key} entry {command!r} is longer than the client takes")
        length = command_line_length(recipe, launch)
        if length > COMMAND_LINE_BYTES:
            raise RecipeError(f"{name}: the command line would be {length} bytes; the game keeps "
                              f"{COMMAND_LINE_BYTES} (sys_cmdline) and cuts the rest, timed list and all. "
                              "Fewer or shorter timed commands, or fewer variants per launch")
        lines = command_line_parts(recipe, launch)
        if lines > MAX_CONSOLE_LINES:
            raise RecipeError(f"{name}: the command line has {lines} '+' parts; the game reads the first "
                              f"{MAX_CONSOLE_LINES} and drops the rest")
        for shot in launch_shots(launch):
            if shot in shots:
                raise RecipeError(f"screenshot {shot!r} is named twice; the second would overwrite the first")
            shots.add(shot)
    pictures = recipe.get("pictures") or {}
    for pair in pictures.get("compare", []):
        for side in ("a", "b"):
            if pair.get(side) not in shots:
                raise RecipeError(f"compare names {pair.get(side)!r}, which no launch takes")
    for name, box in (pictures.get("regions") or {}).items():
        if len(box) != 4 or not (box[0] < box[2] and box[1] < box[3]):
            raise RecipeError(f"region {name}: [x0, y0, x1, y1] with x0 < x1 and y0 < y1")
    return recipe


def command_line_parts(recipe, launch):
    """How many '+' parts the game's command line splitter (Com_ParseCommandLine)
    makes of a launch's arguments: it splits at every '+' and keeps 31."""
    exe = recipe["game"]["exe"]
    skip = len(exe) if isinstance(exe, list) else 1
    return " ".join(launch_argv(recipe, launch, Path("home"))[skip:]).count("+")


def command_line_length(recipe, launch):
    """The length of the command line the game copies into sys_cmdline: the
    arguments after the program, as Windows joins them (Linux joins them the
    same way, src/linux/main.cpp), with a home folder of a typical length."""
    exe = recipe["game"]["exe"]
    skip = len(exe) if isinstance(exe, list) else 1
    home = Path("C:/kisak/runs/run-folder-of-typical-length") / launch["name"] / "home"
    return len(subprocess.list2cmdline(launch_argv(recipe, launch, home)[skip:]))


def check_variants(launch):
    name, variants = launch.get("name"), launch.get("variants")
    if variants is None:
        if launch.get("variant_timing"):
            raise RecipeError(f"{name}: variant_timing without variants")
        return
    if not isinstance(variants, list) or not variants:
        raise RecipeError(f"{name}: variants is a list of {{'name', 'set'}}")
    seen = set()
    for variant in variants:
        label = variant.get("name", "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", label or ""):
            raise RecipeError(f"{name}: variant name {label!r}: letters, digits, _ . - only")
        if label in seen:
            raise RecipeError(f"{name}: variant {label!r} given twice")
        seen.add(label)
        settings = variant.get("set")
        if not isinstance(settings, dict) or not settings:
            raise RecipeError(f"{name}: variant {label!r} sets nothing")
        for dvar, value in settings.items():
            if not DVAR_NAME.match(dvar) or re.search(r"[|;+\"\s]", str(value)) or str(value) == "":
                raise RecipeError(f"{name}: variant {label!r} sets {dvar!r} to {value!r}: one dvar name, "
                                  "one value without spaces, ';', '|', '+' or quotes")
    timing = launch.get("variant_timing") or {}
    for key in timing:
        if key not in ("start", "settle", "framelog", "passes", "compare", "warmup"):
            raise RecipeError(f"{name}: variant_timing has an unknown key {key!r}")
    if timing.get("passes", 1) not in (1, 2):
        raise RecipeError(f"{name}: variant_timing passes is 1 or 2")
    for key in ("start", "settle", "framelog", "warmup"):
        if key in timing and not (isinstance(timing[key], (int, float)) and timing[key] >= 0):
            raise RecipeError(f"{name}: variant_timing {key} is a number of seconds")
    last = max([float(TIMED.match(c).group(1)) for c in launch.get("at_map") or [] if TIMED.match(c)] or [0])
    if "start" in timing and timing["start"] <= last:
        raise RecipeError(f"{name}: variant_timing start {timing['start']} is not after the last at_map command ({last:g})")


def number(value):
    return f"{value:g}"


def variant_plan(launch):
    """The variants' timed commands and what each frame log and screenshot is."""
    variants = launch.get("variants") or []
    if not variants:
        return [], []
    timing = launch.get("variant_timing") or {}
    settle = float(timing.get("settle", DEFAULT_SETTLE))
    framelog = float(timing.get("framelog", 0))
    warmup = float(timing.get("warmup", DEFAULT_WARMUP if framelog else 0))
    passes = int(timing.get("passes", 2 if framelog else 1))
    last = max([float(TIMED.match(c).group(1)) for c in launch.get("at_map") or [] if TIMED.match(c)] or [0])
    t = float(timing.get("start", max(DEFAULT_VARIANT_START, last + 2)))
    order = list(variants) + (list(reversed(variants)) if passes == 2 else [])
    commands, plan = [], []
    for index, variant in enumerate(order):
        number_of_pass = 1 if index < len(variants) else 2
        sets = "; ".join(f"{dvar} {value}" for dvar, value in variant["set"].items())
        commands.append(f"{number(t)} {sets}")
        t += settle + (warmup if index == 0 else 0)
        if number_of_pass == 1:
            shot = f"{launch['name']}-{variant['name']}"
            commands.append(f"{number(t)} screenshot {shot}")
            plan.append({"variant": variant["name"], "pass": 1, "screenshot": shot})
            t += 1
        if framelog:
            label = f"{launch['name']}-{variant['name']}-{number_of_pass}"
            commands.append(f"{number(t)} framelog {number(framelog)} {label}")
            plan.append({"variant": variant["name"], "pass": number_of_pass, "framelog": label})
            t += framelog + 1
    commands.append(f"{number(t)} quit")
    return commands, plan


def launch_commands(launch, key):
    commands = list(launch.get(key) or [])
    if key == "at_map":
        commands += variant_plan(launch)[0]
    return commands


def launch_shots(launch):
    shots = []
    for command in launch_commands(launch, "at_menu") + launch_commands(launch, "at_map"):
        match = SHOT_COMMAND.match(command)
        if match:
            shots.append(match.group(1))
    return shots


def timed_list(commands):
    return " | ".join(command.strip() for command in commands)


def launch_argv(recipe, launch, home):
    game = recipe["game"]
    exe = game["exe"]
    argv = list(exe) if isinstance(exe, list) else [exe]
    argv += list(game.get("args") or [])
    if home:
        argv += ["+set", "fs_homepath", str(home)]
    # The run settings go before the launch's own +devmap/+connect, so the
    # game has them when it reads its command line.
    at_menu, at_map = launch_commands(launch, "at_menu"), launch_commands(launch, "at_map")
    if at_menu:
        argv += ["+set", "run_atMenu", timed_list(at_menu)]
    if at_map:
        argv += ["+set", "run_atMap", timed_list(at_map)]
        if "quit_after_map" not in launch:
            last = max(float(TIMED.match(command).group(1)) for command in at_map)
            argv += ["+set", "run_quitAfterMap", str(int(last) + SAFETY_AFTER_LAST)]
    if launch.get("quit_after_map"):
        argv += ["+set", "run_quitAfterMap", str(int(launch["quit_after_map"]))]
    argv += list(launch.get("args") or [])
    return argv


def launch_env(recipe, launch):
    env = dict(os.environ)
    for pattern in recipe["game"].get("clear_env") or []:
        for key in list(env):
            if fnmatch.fnmatchcase(key, pattern):
                del env[key]
    # No box waits for a click nobody will make: the game answers its
    # questions "No" and prints them (KISAK_NO_DIALOGS, as perf_run.py sets).
    env["KISAK_NO_DIALOGS"] = "1"
    env.update({k: str(v) for k, v in (recipe["game"].get("env") or {}).items()})
    env.update({k: str(v) for k, v in (launch.get("env") or {}).items()})
    return env


# ------------------------------------------------------------ the processes

def running_images():
    """Lower-case process image names, or None where the runner cannot tell."""
    try:
        if os.name == "nt":
            text = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True,
                                  timeout=30).stdout
            return {line.split('","')[0].strip('"').lower() for line in text.splitlines() if line}
        names = set()
        for entry in Path("/proc").iterdir():
            if entry.name.isdigit():
                try:
                    names.add((entry / "comm").read_text().strip().lower())
                except OSError:
                    pass
        return names
    except (OSError, subprocess.SubprocessError):
        return None


def blockers(recipe):
    images = running_images()
    if images is None:
        return []
    exe = recipe["game"]["exe"]
    own = Path(exe[-1] if isinstance(exe, list) else exe).name.lower()
    wanted = [name.lower() for name in recipe["game"].get("forbid_processes") or []]
    found = [name for name in wanted if name in images or name[:15] in images]
    if own and not own.startswith("python") and (own in images or own[:15] in images):
        found.append(own)
    return found


# -------------------------------------------------------------- the launches

def files_under(root, pattern):
    if not root or not Path(root).is_dir():
        return {}
    return {path: path.stat().st_mtime for path in Path(root).rglob(pattern)}


def run_launch(recipe, launch, out, dry_run=False):
    game = recipe["game"]
    folder = out / launch["name"]
    home = None
    if game.get("home_template"):
        home = folder / "home"
    argv = launch_argv(recipe, launch, home)
    result = {"name": launch["name"], "argv": argv, "env": {**(game.get("env") or {}), **(launch.get("env") or {})}}
    if dry_run:
        return result
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    if home:
        shutil.copytree(game["home_template"], home)

    waited = 0
    while blockers(recipe) and waited < 120:
        time.sleep(5)
        waited += 5
    before = blockers(recipe)
    result["blocked_before"] = before
    if before:
        result["status"] = "blocked"
        return result

    search = home or Path(game.get("cwd") or ".")
    shots_before = files_under(search, "*.tga")
    logs_before = files_under(search, "console*.log")
    started = time.time()
    timeout = float(launch.get("timeout") or game.get("timeout") or 300)
    process = subprocess.Popen(argv, cwd=game.get("cwd") or None, env=launch_env(recipe, launch),
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output, _ = process.communicate(timeout=timeout)
        result["status"] = "ok" if process.returncode == 0 else "exit"
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
        result["status"] = "timeout"
    result["exit_code"] = process.returncode
    result["seconds"] = round(time.time() - started, 1)
    (folder / "stdout.txt").write_bytes(output or b"")

    # The log: the console log the launch wrote (logfile 2), else its output.
    log_text = ""
    for path, mtime in sorted(files_under(search, "console*.log").items(), key=lambda item: item[1]):
        if logs_before.get(path) != mtime:
            shutil.copy2(path, folder / path.name)
            log_text += path.read_text(encoding="utf-8", errors="replace")
    if not log_text:
        log_text = (output or b"").decode("utf-8", errors="replace")
    result["lines"] = wanted_lines(log_text, recipe.get("lines") or [])
    result["frame_logs"] = [line for line in log_text.splitlines() if FRAME_LOG.match(line.strip())]
    result["run_errors"] = [line for line in log_text.splitlines() if "none of its commands will be sent" in line]
    if result["run_errors"] and result["status"] == "ok":
        # The game refused the timed list, so nothing the recipe asked for ran:
        # the launch only idled until its safety quit.
        result["status"] = "run_error"
    if launch.get("variants"):
        result["variants"] = variant_summary(launch, result["frame_logs"])

    shots = {}
    for path, mtime in files_under(search, "*.tga").items():
        if shots_before.get(path) != mtime:
            target = folder / path.name
            if path != target:
                shutil.copy2(path, target)
            shots[path.stem] = str(target)
    result["shots"] = shots
    result["missing_shots"] = [name for name in launch_shots(launch) if name not in shots]
    after = blockers(recipe)
    result["blocked_after"] = [name for name in after if name in (game.get("forbid_processes") or [])]
    return result


def variant_summary(launch, frame_logs):
    """Frame rate per variant and pass from the frame log lines, and each
    variant's mean against the first variant's."""
    found = {}
    for line in frame_logs:
        match = FRAME_LOG_FPS.match(line.strip())
        if match:
            found[match.group(1)] = {"fps": float(match.group(4)), "ms_mean": float(match.group(5)),
                                     "ms_median": float(match.group(6)), "ms_p95": float(match.group(7)),
                                     "ms_p99": float(match.group(8))}
    summary = []
    for variant in launch["variants"]:
        entry = {"name": variant["name"], "set": variant["set"], "passes": []}
        for step in variant_plan(launch)[1]:
            if step["variant"] == variant["name"] and "framelog" in step:
                numbers = found.get(step["framelog"])
                entry["passes"].append({"pass": step["pass"], "label": step["framelog"], **(numbers or {"missing": True})})
        fps = [p["fps"] for p in entry["passes"] if "fps" in p]
        if fps:
            entry["mean_fps"] = round(sum(fps) / len(fps), 2)
            medians = [p["ms_median"] for p in entry["passes"] if "ms_median" in p]
            entry["mean_ms_median"] = round(sum(medians) / len(medians), 3)
            if len(fps) > 1:
                entry["pass_spread_pct"] = round(100 * (max(fps) - min(fps)) / (sum(fps) / len(fps)), 2)
        summary.append(entry)
    base = summary[0].get("mean_fps") if summary else None
    for entry in summary:
        if base and entry.get("mean_fps"):
            entry["fps_vs_first_pct"] = round(100 * (entry["mean_fps"] - base) / base, 2)
            entry["ms_vs_first"] = round(1000 / entry["mean_fps"] - 1000 / base, 3)
    return summary


def wanted_lines(text, needles):
    lines = []
    lowered = [needle.lower() for needle in needles]
    for line in text.splitlines():
        low = line.lower()
        if any(needle in low for needle in lowered):
            lines.append(line.rstrip())
    return lines


# --------------------------------------------------------------- the pictures

def read_tga(path):
    """(width, height, rgb bytes top row first) of a true-colour TGA, RLE or not."""
    data = Path(path).read_bytes()
    id_length, colour_map, kind = data[0], data[1], data[2]
    width, height = struct.unpack_from("<HH", data, 12)
    depth, descriptor = data[16], data[17]
    if colour_map or kind not in (2, 10) or depth not in (24, 32):
        raise ValueError(f"{path}: TGA kind {kind}, {depth} bits; only true colour 24/32 is read")
    step = depth // 8
    position = 18 + id_length
    count = width * height
    if kind == 2:
        pixels = data[position:position + count * step]
    else:
        out = bytearray()
        while len(out) < count * step:
            header = data[position]
            position += 1
            run = (header & 0x7F) + 1
            if header & 0x80:
                out += data[position:position + step] * run
                position += step
            else:
                out += data[position:position + run * step]
                position += run * step
        pixels = bytes(out[:count * step])
    rgb = bytearray(count * 3)
    rgb[0::3] = pixels[2::step]
    rgb[1::3] = pixels[1::step]
    rgb[2::3] = pixels[0::step]
    if not descriptor & 0x20:  # bottom row first
        row = width * 3
        rgb = b"".join(bytes(rgb[y * row:(y + 1) * row]) for y in range(height - 1, -1, -1))
    return width, height, bytes(rgb)


def read_png(path):
    """(width, height, rgb bytes top row first) of an 8-bit RGB or RGBA PNG
    without interlacing, as Android's screencap writes it (android_run.py)."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    position, idat, header = 8, bytearray(), None
    while position + 8 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, position)
        body = data[position + 8:position + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        position += 12 + length
    if not header:
        raise ValueError(f"{path}: no IHDR")
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace:
        raise ValueError(f"{path}: PNG depth {depth}, colour type {colour}, interlace {interlace}; "
                         "only 8-bit RGB or RGBA without interlacing is read")
    step = 3 if colour == 2 else 4
    stride = width * step
    raw = zlib.decompress(bytes(idat))
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for i in range(step, stride):
                line[i] = (line[i] + line[i - step]) & 0xFF
        elif kind == 2:
            line = bytearray((a + b) & 0xFF for a, b in zip(line, previous))
        elif kind == 3:
            for i in range(stride):
                left = line[i - step] if i >= step else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - step] if i >= step else 0
                b = previous[i]
                c = previous[i - step] if i >= step else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        elif kind != 0:
            raise ValueError(f"{path}: row {y} has filter {kind}")
        rows.append(bytes(line))
        previous = line
    pixels = b"".join(rows)
    if step == 3:
        return width, height, pixels
    rgb = bytearray(width * height * 3)
    rgb[0::3] = pixels[0::4]
    rgb[1::3] = pixels[1::4]
    rgb[2::3] = pixels[2::4]
    return width, height, bytes(rgb)


def read_picture(path):
    """A screenshot the game wrote (TGA) or a screen capture (PNG)."""
    return read_png(path) if str(path).lower().endswith(".png") else read_tga(path)


def write_png(path, width, height, rgb):
    row = width * 3
    raw = b"".join(b"\x00" + rgb[y * row:(y + 1) * row] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def region_mean(image, box):
    width, height, rgb = image
    x0, y0, x1, y1 = clamp_box(box, width, height)
    sums = [0, 0, 0]
    for y in range(y0, y1):
        line = rgb[(y * width + x0) * 3:(y * width + x1) * 3]
        sums[0] += sum(line[0::3])
        sums[1] += sum(line[1::3])
        sums[2] += sum(line[2::3])
    pixels = max(1, (x1 - x0) * (y1 - y0))
    return [round(value / pixels, 1) for value in sums]


def clamp_box(box, width, height):
    x0, y0, x1, y1 = box
    return max(0, x0), max(0, y0), min(width, x1), min(height, y1)


def compare(a, b, levels, box=None, tile=32):
    """Numbers for two pictures of the same size; the difference of a pixel is
    its largest channel difference."""
    width, height, ra = a
    if (b[0], b[1]) != (width, height):
        raise ValueError(f"sizes differ: {width}x{height} and {b[0]}x{b[1]}")
    rb = b[2]
    x0, y0, x1, y1 = clamp_box(box or (0, 0, width, height), width, height)
    over = {level: 0 for level in levels}
    total = 0
    signed = 0
    top = max(levels)
    tiles = {}
    for y in range(y0, y1):
        base = (y * width) * 3
        for x in range(x0, x1):
            i = base + x * 3
            dr, dg, db = rb[i] - ra[i], rb[i + 1] - ra[i + 1], rb[i + 2] - ra[i + 2]
            d = max(abs(dr), abs(dg), abs(db))
            total += d
            signed += dr + dg + db
            for level in levels:
                if d > level:
                    over[level] += 1
            if d > top:
                key = (x // tile, y // tile)
                tiles[key] = tiles.get(key, 0) + 1
    pixels = max(1, (x1 - x0) * (y1 - y0))
    return {
        "box": [x0, y0, x1, y1],
        "mean_abs": round(total / pixels, 2),
        "mean_signed_b_minus_a": round(signed / pixels / 3, 2),
        "share_over": {str(level): round(over[level] / pixels, 4) for level in levels},
        "boxes_over_top": change_boxes(tiles, tile, width, height),
    }


def change_boxes(tiles, tile, width, height, limit=5):
    """Boxes around connected tiles holding changed pixels, biggest first."""
    seen, boxes = set(), []
    for start in tiles:
        if start in seen:
            continue
        stack, members = [start], []
        seen.add(start)
        while stack:
            tx, ty = stack.pop()
            members.append((tx, ty))
            for nx, ny in ((tx + 1, ty), (tx - 1, ty), (tx, ty + 1), (tx, ty - 1)):
                if (nx, ny) in tiles and (nx, ny) not in seen:
                    seen.add((nx, ny))
                    stack.append((nx, ny))
        xs, ys = [m[0] for m in members], [m[1] for m in members]
        boxes.append({"box": [min(xs) * tile, min(ys) * tile, min(width, (max(xs) + 1) * tile),
                              min(height, (max(ys) + 1) * tile)],
                      "pixels": sum(tiles[m] for m in members)})
    boxes.sort(key=lambda entry: -entry["pixels"])
    return boxes[:limit]


def half(image):
    width, height, rgb = image
    w, h = width // 2, height // 2
    out = bytearray(w * h * 3)
    for y in range(h):
        source = (2 * y * width) * 3
        line = rgb[source:source + width * 3]
        out[y * w * 3:(y + 1) * w * 3] = b"".join(line[x * 6:x * 6 + 3] for x in range(w))
    return w, h, bytes(out)


def side_by_side(a, b, gap=8):
    (wa, ha, ra), (wb, hb, rb) = a, b
    width, height = wa + gap + wb, max(ha, hb)
    out = bytearray(width * height * 3)
    for y in range(height):
        row = y * width * 3
        if y < ha:
            out[row:row + wa * 3] = ra[y * wa * 3:(y + 1) * wa * 3]
        if y < hb:
            start = row + (wa + gap) * 3
            out[start:start + wb * 3] = rb[y * wb * 3:(y + 1) * wb * 3]
    return width, height, bytes(out)


def diff_picture(a, b, scale=4):
    width, height, ra = a
    rb = b[2]
    return width, height, bytes(min(255, abs(x - y) * scale) for x, y in zip(ra, rb))


def crop(image, box):
    width, height, rgb = image
    x0, y0, x1, y1 = clamp_box(box, width, height)
    return x1 - x0, y1 - y0, b"".join(rgb[(y * width + x0) * 3:(y * width + x1) * 3] for y in range(y0, y1))


def compare_pairs(recipe):
    """The recipe's own pairs, then each launch's first variant against each
    other variant when its variant_timing asks for "compare"."""
    pairs = list((recipe.get("pictures") or {}).get("compare") or [])
    for launch in recipe["launches"]:
        variants = launch.get("variants") or []
        if variants and (launch.get("variant_timing") or {}).get("compare"):
            first = f"{launch['name']}-{variants[0]['name']}"
            for variant in variants[1:]:
                pairs.append({"a": first, "b": f"{launch['name']}-{variant['name']}",
                              "label": f"{launch['name']}: {variants[0]['name']} vs {variant['name']}"})
    return pairs


def pictures(recipe, results, out):
    settings = recipe.get("pictures") or {}
    levels = sorted(settings.get("levels") or [2, 16])
    regions = settings.get("regions") or {}
    shots = {name: path for launch in results for name, path in (launch.get("shots") or {}).items()}
    cache = {}

    def image(name):
        if name not in cache:
            cache[name] = read_picture(shots[name])
        return cache[name]

    report = {"means": {}, "compare": []}
    for name in sorted(shots):
        try:
            img = image(name)
        except (OSError, ValueError) as error:
            report["means"][name] = {"error": str(error)}
            continue
        entry = {"size": [img[0], img[1]], "whole": region_mean(img, (0, 0, img[0], img[1]))}
        for region, box in regions.items():
            entry[region] = region_mean(img, box)
        report["means"][name] = entry
    folder = out / "pictures"
    folder.mkdir(exist_ok=True)
    for pair in compare_pairs(recipe):
        a, b = pair["a"], pair["b"]
        entry = {"a": a, "b": b, "label": pair.get("label", f"{a} vs {b}")}
        if a not in shots or b not in shots:
            entry["error"] = "missing screenshot"
            report["compare"].append(entry)
            continue
        try:
            ia, ib = image(a), image(b)
            entry["whole"] = compare(ia, ib, levels)
            for region, box in regions.items():
                entry[region] = compare(ia, ib, levels, box)
            stem = f"{a}-vs-{b}"
            write_png(folder / f"pair-{stem}.png", *side_by_side(half(ia), half(ib)))
            write_png(folder / f"diff-{stem}.png", *diff_picture(ia, ib))
            biggest = entry["whole"]["boxes_over_top"]
            if biggest:
                x0, y0, x1, y1 = biggest[0]["box"]
                pad = 32
                box = (x0 - pad, y0 - pad, x1 + pad, y1 + pad)
                write_png(folder / f"crop-{stem}.png", *side_by_side(crop(ia, box), crop(ib, box)))
            entry["files"] = sorted(path.name for path in folder.glob(f"*-{stem}.png"))
        except (OSError, ValueError) as error:
            entry["error"] = str(error)
        report["compare"].append(entry)
    return report


# ----------------------------------------------------------------- the report

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def seal(results):
    body = {key: value for key, value in results.items() if key != "sha256"}
    results["sha256"] = hashlib.sha256(canonical(body).encode("ascii")).hexdigest()
    return results


def launch_report_lines(launch):
    """A launch's variant table and its verbatim lines (android_run.py too)."""
    lines = []
    if launch.get("variants"):
        lines += ["| Variant | Sets | fps per pass | Mean fps | vs first | ms vs first | Pass spread |",
                  "| --- | --- | --- | --- | --- | --- | --- |"]
        for entry in launch["variants"]:
            sets = ", ".join(f"{dvar} {value}" for dvar, value in entry["set"].items())
            per_pass = ", ".join(str(p.get("fps", "missing")) for p in entry["passes"]) or "-"
            vs, ms = entry.get("fps_vs_first_pct"), entry.get("ms_vs_first")
            spread = entry.get("pass_spread_pct")
            lines.append(f"| {entry['name']} | {sets} | {per_pass} | {entry.get('mean_fps', '-')} | "
                         f"{'-' if vs is None else f'{vs:+.2f}%'} | {'-' if ms is None else f'{ms:+.3f}'} | "
                         f"{'-' if spread is None else f'{spread}%'} |")
        wide = [e["name"] for e in launch["variants"] if (e.get("pass_spread_pct") or 0) > SPREAD_WARNING_PCT]
        if wide:
            lines.append(f"- passes differ by more than {SPREAD_WARNING_PCT}% for {', '.join(wide)}: "
                         "the launch drifted, or a pass ran during a change of scene")
        lines.append("")
    verbatim = (launch.get("frame_logs") or []) + (launch.get("lines") or [])
    if verbatim:
        lines += ["```"] + verbatim + ["```"]
    return lines


def picture_report_lines(pics):
    """The mean colours and the comparisons (android_run.py too)."""
    lines = []
    if pics.get("means"):
        regions = sorted({key for entry in pics["means"].values() for key in entry if key not in ("size", "error")})
        lines += ["", "## Mean colour (R, G, B)", "", "| Screenshot | " + " | ".join(regions) + " |",
                  "| --- |" + " --- |" * len(regions)]
        for name, entry in sorted(pics["means"].items()):
            lines.append(f"| {name} | " + " | ".join(str(entry.get(region, "-")) for region in regions) + " |")
    for entry in pics.get("compare") or []:
        lines += ["", f"## {entry['label']}", ""]
        if entry.get("error"):
            lines.append(f"Not compared: {entry['error']}.")
            continue
        for region, numbers in entry.items():
            if not isinstance(numbers, dict) or "mean_abs" not in numbers:
                continue
            shares = ", ".join(f"> {level}: {share * 100:.2f}%" for level, share in numbers["share_over"].items())
            boxes = "; ".join(f"{b['box']} ({b['pixels']} px)" for b in numbers["boxes_over_top"]) or "none"
            lines.append(f"- {region} {numbers['box']}: mean |diff| {numbers['mean_abs']}, mean b - a "
                         f"{numbers['mean_signed_b_minus_a']}, {shares}; changed areas: {boxes}")
        if entry.get("files"):
            lines.append(f"- pictures: {', '.join(entry['files'])}")
    return lines


def report_text(results):
    lines = [f"# {results['title']}", "", f"Recipe {results['id']}, run {results['finished']}. "
             "Local run on a test machine, not CI evidence.", ""]
    lines += ["| Launch | Status | Exit | Seconds | Screenshots | Missing |", "| --- | --- | --- | --- | --- | --- |"]
    for launch in results["launches"]:
        lines.append(f"| {launch['name']} | {launch.get('status', '-')} | {launch.get('exit_code', '-')} | "
                     f"{launch.get('seconds', '-')} | {len(launch.get('shots') or {})} | "
                     f"{', '.join(launch.get('missing_shots') or []) or '-'} |")
    for launch in results["launches"]:
        lines += ["", f"## {launch['name']}", ""]
        for key in ("blocked_before", "blocked_after", "run_errors"):
            if launch.get(key):
                lines.append(f"- {key.replace('_', ' ')}: {', '.join(launch[key])}")
        lines += launch_report_lines(launch)
    lines += picture_report_lines(results.get("pictures") or {})
    lines += ["", f"results.json sha256 {results['sha256']}", ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("recipe")
    parser.add_argument("--out", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--pictures-only", action="store_true")
    options = parser.parse_args(argv)
    try:
        recipe = load_recipe(options.recipe)
    except RecipeError as error:
        print(f"pc_run: {error}", file=sys.stderr)
        return 2
    out = Path(options.out)
    if recipe["game"].get("home_template") and "+" in str(out.resolve()):
        print(f"pc_run: the output folder {out.resolve()} has a '+'; each launch's fs_homepath is under it and "
              "the game would split its command line there", file=sys.stderr)
        return 2
    if options.dry_run:
        for launch in recipe["launches"]:
            result = run_launch(recipe, launch, out, dry_run=True)
            print(f"{launch['name']}: env {result['env']}\n  {subprocess.list2cmdline(result['argv'])}")
        return 0
    out.mkdir(parents=True, exist_ok=True)
    previous = {}
    results_path = out / "results.json"
    if (options.resume or options.pictures_only) and results_path.exists():
        previous = {launch["name"]: launch for launch in json.loads(results_path.read_text())["launches"]}
    launches = []
    for launch in recipe["launches"]:
        old = previous.get(launch["name"])
        if options.pictures_only or (old and old.get("status") == "ok" and not old.get("missing_shots")):
            launches.append(old or {"name": launch["name"], "status": "not run"})
            continue
        print(f"pc_run: {launch['name']} ...", flush=True)
        result = run_launch(recipe, launch, out)
        print(f"pc_run: {launch['name']}: {result.get('status')} in {result.get('seconds', 0)} s, "
              f"{len(result.get('shots') or {})} screenshots", flush=True)
        launches.append(result)
        if (out / "PAUSE").exists():
            print("pc_run: PAUSE found; stopping before the next launch (--resume goes on)")
            break
    results = {"format": FORMAT, "id": recipe.get("id", ""), "title": recipe.get("title", recipe.get("id", "")),
               "finished": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "launches": launches}
    results["pictures"] = pictures(recipe, launches, out)
    seal(results)
    results_path.write_text(json.dumps(results, indent=1, sort_keys=True))
    (out / "report.md").write_text(report_text(results), encoding="utf-8")
    print(f"pc_run: report {out / 'report.md'}")
    ok = all(launch.get("status") == "ok" and not launch.get("missing_shots") for launch in launches)
    ok = ok and len(launches) == len(recipe["launches"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
