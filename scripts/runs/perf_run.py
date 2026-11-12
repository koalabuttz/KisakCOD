#!/usr/bin/env python3
"""One driver for frame-rate runs of the native client on a test machine.

Not a CI check: it needs a Call of Duty 4 install and a GPU session, and runs on
a machine that has both (docs/runs.md). A run is described by one recipe file:
the game binary and its arguments, the variants to compare (environment and
arguments), the order of the launches, the scene as console lines, and the log
lines wanted. The driver launches each row, plays the scene through the game's
standard input, measures with the ``framelog`` console command, and writes

    <out>/results.json   everything, checksummed (scripts/runs/run_report.py)
    <out>/results-compact.json  its short form, to paste when that is too long
    <out>/report.md      the tables and the verbatim lines
    <out>/<row>.log      each launch's console output (stays on the machine)

It replaces the per-run copies of launch, steps, driver, export and guard
scripts, and the hand-written report.

What it checks by itself, because each has spoiled a row before:

* power: the AC state the round needs (or the state the first row had), the
  battery floor before a launch, the power profile, and the AC state once a
  second while a capture runs. A row whose power changed is marked and run
  again (``retries``), after waiting for the state to come back;
* suspend: a machine that slept while a row ran (a closed lid) wakes up and
  carries on as if nothing happened, with a row whose waits and frame times
  are wrong. Such a row is marked and run again like one whose power changed;
* other load: before a launch and again before a capture it waits while
  another process uses more than a share of one core, and it measures the
  other processes over the capture itself. A row captured beside a busy
  process is marked and run again like one whose power changed (on the
  laptop a process that was not ours started inside a row and cost it a
  sixth of its frame rate, and the row read "ok");
* leftovers: no launch while a process of the same binary still runs;
* the guard: listings (names, sizes, times; no contents) of the folders named
  in the recipe are compared before and after every row;
* a file named PAUSE in the output folder stops the run before the next launch.

``--resume`` skips rows whose results are already in results.json, so a run
stopped by power or by PAUSE picks up where it left off.

The recipe (JSON)::

    {
      "format": "kisak-run/1",
      "id": "vulkan-laptop-run-11",
      "title": "Vulkan back end, laptop run 11: the feed interval",
      "asker": "Develop the Vulkan renderer",
      "game": {
        "binary": "~/kisak-build/vk-mp-x64-feed/bin/KisakCOD-mp",
        "args": ["+set", "fs_basepath", "~/games/cod4", "+set", "logfile", "2"],
        "env": {"LD_LIBRARY_PATH": "~/kisak-sdk/dxvk-x64/lib:~/kisak-sdk/sdl3-x64/lib"},
        "clear_env": ["KISAK_VK_*"],
        "home_template": "~/kisak-private/x64-run9/home"
      },
      "variants": {
        "A": {},
        "D": {"args": ["+set", "r_backend", "d3d9"]},
        "F64": {"env": {"KISAK_VK_GPU_FEED_DRAWS": "64"}}
      },
      "rows": ["A", "D", "F64", "F64", "D", "A"],
      "scene": [
        {"wait_quiet": 15, "min": 20, "timeout": 150},
        {"send": "sv_serverid"},
        {"expect": "\\"sv_serverid\\" is: \\"(?P<serverid>\\\\d+)", "timeout": 10},
        {"send": "mr {serverid} 8 autoassign"},
        {"sleep": 3},
        {"send": "set scr_testclients 12"},
        {"poll": "status", "count": "bot\\\\d+\\\\^7", "at_least": 12, "timeout": 120, "name": "bots"},
        {"framelog": 120}
      ],
      "lines": ["^Vulkan: legacy: device ready:", "frame stats:"],
      "compact_lines": 0,
      "counts": {"validation": "validation error"},
      "power": {"require": "same", "battery_floor": 10, "battery_per_launch": 8,
                "profile": "power-saver", "wait": 300, "retries": 1},
      "load": {"max_cpu_percent": 50, "wait": 600},
      "stop_if_running": ["Vita3K"],
      "guard": ["~/games/cod4", "~/games/cod4/main", "~/games/cod4/players/**"]
    }

A variant may name a ``"binary"`` of its own (two builds compared in one run);
each row notes the SHA-256 and the time of the binary it ran. ``game`` may also
carry ``"args_after"`` (arguments that follow the variant's,
such as ``+devmap``), ``"wrap": ["taskset", "-c", "0,1"]`` (a command the game
is started through), ``"cwd"`` and ``"quit_timeout"``. A guard folder
ending in ``/**`` is listed with everything below it. ``compact_lines`` is how many of a
row's wanted lines per pattern the short form of the results carries (0: none).

``~`` and ``{name}`` are expanded in paths, arguments, environment values and
console lines: ``{row}``, ``{variant}``, ``{out}``, ``{home}``, and every named
group an ``expect`` step matched.

A recipe kept in the repository (scripts/runs/recipes/) names no machine's
folders: it writes ``{game}``, ``{build}`` and the like, and each machine gives
those once in a names file, a JSON object such as ``{"game": "~/games/cod4"}``.
The file is the one ``--names`` gives, else the one the environment variable
KISAK_RUN_NAMES gives, else ``~/.config/kisak/run-names.json``; ``--set
name=value`` adds to it or overrides it for one run. A recipe that uses a name
nobody gave is refused before anything starts.

Scene steps:

``{"wait_quiet": s, "min": m, "timeout": t}``  no output for s seconds, and at
    least m seconds since the launch (the load is over)
``{"send": "line"}``                 one console line; a list sends each, with
    ``"gap"`` seconds between them (0.3 by default)
``{"expect": "regex", "timeout": t}``  a log line; named groups become names
    and are kept with the row
``{"sleep": s}``                     keep reading for s seconds
``{"poll": "line", "count": "regex", "at_least": n, "every": s, "timeout": t,
"name": n}``  send the line every s seconds until at least n of the lines that
    answer it match; the last count is kept with the row under the name
``{"framelog": s}``                  ``framelog s <row>`` and its summary line;
    ``"with": {"run": [argv], ...}`` runs a tool for the same stretch (a
    profiler capture)
``{"run": [argv], "timeout": t, "name": n}``  a tool beside the game; its last
    output lines go into the row as ``n``

A step may carry ``"optional": true``: its failure is noted and the row goes on.
``after`` in the recipe is a list of ``run`` steps for after the quit (an
export), so nothing competes with the game for the CPU.

Exit status: 0 when every row ran and none failed, 1 when a row failed or the
run stopped early, 2 on a usage error or a recipe that cannot be read.

Standard library only; Linux.
"""

import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import queue
import re
import shlex
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_report  # noqa: E402

RECIPE_FORMAT = "kisak-run/1"
FRAME_LOG = run_report.FRAME_LOG
STANDARD_COUNTS = {
    "errors": re.compile(r"^(ERROR: |FATAL ERROR: |Sys_Error: )"),
    "warnings": re.compile(r"^(\^3)?WARNING", re.I),
    "asserts": re.compile(r"ASSERT FAILED: "),
}
POWER_SUPPLY = Path("/sys/class/power_supply")
PROC = Path("/proc")
TAIL_LINES = 15
STEP_KINDS = ("wait_quiet", "send", "expect", "sleep", "poll", "framelog", "run")


class RecipeError(Exception):
    pass


class StopRun(Exception):
    """The run cannot go on (power, PAUSE); rows done so far are kept."""


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# --- the machine's state ----------------------------------------------------------------------------

def read_power(root=None):
    """{'ac': bool or None, 'battery': percent or None} from the kernel's power
    supplies. None means the machine does not say (a desktop has no battery)."""
    root = Path(root) if root else POWER_SUPPLY
    ac, battery = None, None
    try:
        supplies = sorted(root.iterdir())
    except OSError:
        supplies = []
    for supply in supplies:
        try:
            kind = (supply / "type").read_text().strip()
        except OSError:
            continue
        try:
            if kind == "Mains" or kind.startswith("USB"):
                online = (supply / "online").read_text().strip() == "1"
                ac = online if ac is None else (ac or online)
            elif kind == "Battery" and battery is None:
                battery = int((supply / "capacity").read_text().strip())
        except (OSError, ValueError):
            continue
    return {"ac": ac, "battery": battery}


def slept_seconds():
    """Seconds this machine has spent suspended since it booted (Linux: the
    boot-time clock counts them, the monotonic clock does not). 0.0 where the
    system cannot say."""
    try:
        awake = time.clock_gettime(time.CLOCK_MONOTONIC)
        return max(0.0, time.clock_gettime(time.CLOCK_BOOTTIME) - awake)
    except (AttributeError, OSError):
        return 0.0


def read_profile():
    """The power profile ("power-saver", "balanced", ...) or None when the
    machine has no powerprofilesctl."""
    try:
        done = subprocess.run(["powerprofilesctl", "get"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout.strip() or None


def _process_times(root=None):
    """{pid: (name, cpu ticks)} of every process."""
    root = Path(root) if root else PROC
    times = {}
    try:
        entries = list(root.iterdir())
    except OSError:
        return times
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        # The name is in parentheses and may hold spaces or parentheses itself.
        open_at, close_at = stat.find("("), stat.rfind(")")
        fields = stat[close_at + 2:].split()
        try:
            times[int(entry.name)] = (stat[open_at + 1:close_at], int(fields[11]) + int(fields[12]))
        except (IndexError, ValueError):
            continue
    return times


def sample_load(seconds=2.0, exclude=(), root=None):
    """The busiest other processes over a short window: [(name, percent of one
    core)], busiest first. exclude: pids to leave out, with the driver itself."""
    ticks = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100
    skip = set(exclude) | {os.getpid()}
    before = _process_times(root)
    time.sleep(seconds)
    after = _process_times(root)
    load = []
    for pid, (name, used) in after.items():
        if pid in skip:
            continue
        # A process that began inside the window did all its work inside it.
        share = 100.0 * (used - before.get(pid, (name, 0))[1]) / ticks / seconds
        if share >= 1.0:
            load.append((name, round(share, 1)))
    load.sort(key=lambda item: -item[1])
    return load


def running_copies(binary, root=None):
    """Pids of processes whose executable is this binary (leftovers of an
    earlier row). Reads the exe link, so a shell whose command line merely
    names the binary does not count."""
    root = Path(root) if root else PROC
    target = os.path.realpath(binary)
    pids = []
    try:
        entries = list(root.iterdir())
    except OSError:
        return pids
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            if os.path.realpath(os.readlink(entry / "exe")) == target:
                pids.append(int(entry.name))
        except OSError:
            continue
    return pids


def running_names(names, root=None):
    """Which of these process names run now (another emulator, a build)."""
    wanted = set(names)
    return sorted({name for name, _ in _process_times(root).values() if name in wanted})


def binary_identity(path):
    """Which build a row ran: the binary's SHA-256 (shortened) and its time."""
    try:
        digest = hashlib.sha256()
        with open(path, "rb") as file:
            for block in iter(lambda: file.read(1 << 20), b""):
                digest.update(block)
        modified = datetime.datetime.fromtimestamp(os.path.getmtime(path), datetime.timezone.utc)
        return {"sha256": digest.hexdigest()[:16], "modified": modified.strftime("%Y-%m-%dT%H:%M:%SZ")}
    except OSError:
        return {}


def snapshot(folders):
    """Names, sizes and modification times of what the guard's folders hold
    directly, or with everything below them for a folder written "folder/**".
    No contents are read."""
    listing = {}

    def one(folder, deep):
        try:
            with os.scandir(folder) as entries:
                for entry in entries:
                    try:
                        info = entry.stat(follow_symlinks=False)
                    except OSError:
                        continue
                    is_dir = entry.is_dir(follow_symlinks=False)
                    size = info.st_size if entry.is_file(follow_symlinks=False) else 0
                    listing[os.path.join(folder, entry.name)] = (size, int(info.st_mtime))
                    if deep and is_dir:
                        one(os.path.join(folder, entry.name), True)
        except OSError as error:
            listing[folder] = ("unreadable", str(error))

    for folder in folders:
        deep = folder.endswith("/**")
        one(folder[:-3] if deep else folder, deep)
    return listing


def compare_snapshots(before, after):
    added = sorted(set(after) - set(before))
    removed = sorted(set(before) - set(after))
    changed = sorted(name for name in set(before) & set(after) if before[name] != after[name])
    if not (added or removed or changed):
        return "ok"
    parts = []
    for word, names in (("added", added), ("removed", removed), ("changed", changed)):
        if names:
            shown = ", ".join(os.path.basename(name) for name in names[:5])
            parts.append(f"{len(names)} {word} ({shown}{', ...' if len(names) > 5 else ''})")
    return "changed: " + "; ".join(parts)


# --- the game process -------------------------------------------------------------------------------

class Game:
    """The client, fed through its standard input, its output read line by line."""

    def __init__(self, command, cwd, env, log_path):
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        self.log.write("$ " + " ".join(shlex.quote(part) for part in command) + "\n")
        self.log.flush()
        self.queue = queue.Queue()
        self.start = time.monotonic()
        self.lines = []
        self.ended = False
        self.process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, errors="replace", bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.queue.put(line.rstrip("\r\n"))
        self.queue.put(None)

    def elapsed(self):
        return time.monotonic() - self.start

    def send(self, line):
        self.log.write(f"[driver +{self.elapsed():.1f}s] > {line}\n")
        self.log.flush()
        try:
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()
            return True
        except (BrokenPipeError, OSError, ValueError):
            return False

    def _take(self, timeout):
        """One line, or None when none came in time or the output ended."""
        if self.ended:
            time.sleep(min(timeout, 0.05))
            return None
        try:
            line = self.queue.get(timeout=timeout)
        except queue.Empty:
            return None
        if line is None:
            self.ended = True
            return None
        self.log.write(line + "\n")
        self.lines.append(line)
        return line

    def wait_for(self, pattern, timeout):
        """Reads until a line matches; None on timeout or when the output ends."""
        deadline = time.monotonic() + timeout
        while not self.ended:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            line = self._take(min(remaining, 0.5))
            if line is not None and pattern is not None:
                match = pattern.search(line)
                if match:
                    return match
        return None

    def wait_quiet(self, quiet, timeout, least=0.0):
        """True once no line came for `quiet` seconds, after at least one did
        and no sooner than `least` seconds after the launch."""
        deadline = time.monotonic() + timeout
        last = time.monotonic() if self.lines else None
        while not self.ended and time.monotonic() < deadline:
            if self._take(0.25) is not None:
                last = time.monotonic()
            elif last is not None and time.monotonic() - last >= quiet and self.elapsed() >= least:
                return True
        return False

    def count_for(self, pattern, seconds):
        """How many of the lines of the next `seconds` match."""
        deadline = time.monotonic() + seconds
        count = 0
        while not self.ended:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            line = self._take(min(remaining, 0.25))
            if line is not None and pattern.search(line):
                count += 1
        return count

    def alive(self):
        return not self.ended and self.process.poll() is None

    def finish(self, timeout):
        """Asks the game to quit; its exit status, or None if it had to be killed."""
        if self.process.poll() is None:
            self.send("quit")
        deadline = time.monotonic() + timeout
        while self.process.poll() is None and time.monotonic() < deadline:
            self._take(0.25)
        status = self.process.poll()
        if status is None:
            self.log.write(f"[driver] no exit {timeout} s after quit; killing\n")
            self.process.kill()
            try:
                self.process.wait(10)
            except subprocess.TimeoutExpired:
                pass
        drain = time.monotonic() + 2
        while not self.ended and time.monotonic() < drain:
            self._take(0.1)
        self.log.close()
        try:
            self.process.stdin.close()
        except OSError:
            pass
        self.reader.join(5)
        return status


class PowerWatch:
    """Notes the AC state once a second while a capture runs."""

    def __init__(self, interval=1.0):
        self.interval = interval
        self.first = read_power()["ac"]
        self.changed = False
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._watch, daemon=True)
        self._thread.start()

    def _watch(self):
        while not self._stop.wait(self.interval):
            if read_power()["ac"] != self.first:
                self.changed = True

    def stop(self):
        self._stop.set()
        self._thread.join(5)
        if read_power()["ac"] != self.first:
            self.changed = True
        return self.changed


# --- the recipe -------------------------------------------------------------------------------------

def expand(text, names):
    """~ and {name} in a recipe string. Unknown names stay as written, so a
    console line with braces of its own passes through."""
    def replace(match):
        return str(names.get(match.group(1), match.group(0)))
    text = re.sub(r"\{(\w+)\}", replace, text)
    if text.startswith("~"):
        text = os.path.expanduser(text)
    return re.sub(r"(?<=[:=])~(?=/)", os.path.expanduser("~"), text)


def load_recipe(path):
    try:
        recipe = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RecipeError(f"{path}: {error}") from error
    if not isinstance(recipe, dict) or recipe.get("format") != RECIPE_FORMAT:
        raise RecipeError(f"{path}: not a {RECIPE_FORMAT} recipe")
    for key in ("id", "game", "rows", "scene"):
        if key not in recipe:
            raise RecipeError(f"{path}: the recipe has no \"{key}\"")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", str(recipe["id"])):
        raise RecipeError(f"{path}: the id may hold letters, digits, dots, dashes and underscores only")
    if "binary" not in recipe["game"]:
        raise RecipeError(f"{path}: \"game\" names no binary")
    variants = recipe.setdefault("variants", {})
    if not recipe["rows"]:
        raise RecipeError(f"{path}: no rows")
    for name in recipe["rows"]:
        if name not in variants:
            raise RecipeError(f"{path}: row \"{name}\" is not one of the variants ({', '.join(sorted(variants))})")
        if not re.fullmatch(r"[A-Za-z0-9._]+", name):
            raise RecipeError(f"{path}: variant \"{name}\" may hold letters, digits, dots and underscores only")
    kinds = set(STEP_KINDS)
    for index, step in enumerate(list(recipe["scene"]) + list(recipe.get("after", [])), 1):
        found = [kind for kind in kinds if kind in step] if isinstance(step, dict) else []
        if len(found) != 1:
            raise RecipeError(f"{path}: step {index} must be exactly one of {', '.join(sorted(kinds))}")
        for key in ("expect", "count"):
            if key in step:
                try:
                    re.compile(step[key])
                except re.error as error:
                    raise RecipeError(f"{path}: step {index}: {error}") from error
        if "poll" in step and "count" not in step:
            raise RecipeError(f"{path}: step {index}: a poll step needs \"count\"")
        if "with" in step and ("framelog" not in step or "run" not in step["with"]):
            raise RecipeError(f"{path}: step {index}: \"with\" belongs to a framelog step and holds a run step")
    for step in recipe.get("after", []):
        if "run" not in step:
            raise RecipeError(f"{path}: \"after\" takes run steps only")
    for pattern in list(recipe.get("lines", [])) + list(recipe.get("counts", {}).values()):
        try:
            re.compile(pattern)
        except re.error as error:
            raise RecipeError(f"{path}: pattern {pattern!r}: {error}") from error
    require = recipe.get("power", {}).get("require", "any")
    if require not in ("any", "same", "mains", "battery"):
        raise RecipeError(f"{path}: power.require is any, same, mains or battery")
    return recipe


ROW_NAMES = ("row", "variant", "out", "home")


def missing_names(recipe, names):
    """The {names} the recipe's launch settings use that neither the machine's
    names nor the driver give. Scene lines are left out: they may use what an
    expect step reads from the log."""
    game = recipe["game"]
    texts = [game.get("binary", ""), game.get("home_template", ""), game.get("cwd", "")]
    texts += [(spec or {}).get("binary", "") for spec in recipe.get("variants", {}).values()]
    texts += list(recipe.get("guard", []))
    holders = [game] + [spec or {} for spec in recipe.get("variants", {}).values()]
    for holder in holders:
        texts += list(holder.get("args", [])) + list(holder.get("args_after", [])) + list(holder.get("wrap", []))
        texts += list((holder.get("env") or {}).values())
    known = set(names) | set(ROW_NAMES)
    used = set()
    for text in texts:
        used.update(re.findall(r"\{(\w+)\}", str(text)))
    return sorted(used - known)


def load_names(path=None, sets=()):
    """The machine's names: the names file, then the --set pairs over it."""
    explicit = path or os.environ.get("KISAK_RUN_NAMES")
    file = Path(os.path.expanduser(explicit)) if explicit else Path.home() / ".config" / "kisak" / "run-names.json"
    names = {}
    if explicit or file.exists():
        try:
            names = json.loads(file.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise RecipeError(f"{file}: {error}") from error
        if not isinstance(names, dict):
            raise RecipeError(f"{file}: a names file is one JSON object")
    for pair in sets:
        name, equals, value = pair.partition("=")
        if not equals or not re.fullmatch(r"\w+", name):
            raise RecipeError(f"--set takes name=value, not \"{pair}\"")
        names[name] = value
    return {name: expand(str(value), {}) for name, value in names.items()}


def row_names(rows):
    """A-1, D-1, A-2: the variant and which launch of it this is."""
    seen, names = {}, []
    for variant in rows:
        seen[variant] = seen.get(variant, 0) + 1
        names.append((f"{variant}-{seen[variant]}", variant))
    return names


# --- one run ----------------------------------------------------------------------------------------

class Run:
    def __init__(self, recipe, recipe_text, out, echo=print, names=None):
        self.recipe = recipe
        self.out = Path(out)
        self.echo = echo
        self.names = dict(names or {})
        self.power_rules = recipe.get("power", {})
        self.load_rules = recipe.get("load", {})
        self.round_ac = None
        self.baseline = None
        self.guard = [expand(folder, self.names) for folder in recipe.get("guard", [])]
        self.results = {
            "format": run_report.RESULT_FORMAT,
            "id": recipe["id"],
            "title": recipe.get("title", recipe["id"]),
            "asker": recipe.get("asker", ""),
            "kind": "perf",
            "machine": socket.gethostname(),
            "recipe_sha256": hashlib.sha256(recipe_text.encode("utf-8")).hexdigest(),
            # How many wanted lines per pattern the short form keeps (0: none).
            "compact_lines": int(recipe.get("compact_lines", 0)),
            "started": now_utc(),
            "finished": "",
            "status": "running",
            "rows": [],
        }

    # the results file is rewritten after every row, so a stop loses nothing
    def save(self):
        self.results["finished"] = now_utc()
        sealed = run_report.seal(self.results)
        (self.out / "results.json").write_text(json.dumps(sealed, indent=1) + "\n", encoding="utf-8")
        short = json.dumps(run_report.compact(sealed), sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        (self.out / "results-compact.json").write_text(short + "\n", encoding="utf-8")
        (self.out / "report.md").write_text(run_report.render(sealed), encoding="utf-8")

    def adopt(self, earlier):
        """--resume: keep the rows an earlier, stopped run finished."""
        if earlier.get("recipe_sha256") != self.results["recipe_sha256"]:
            raise RecipeError("results.json in the output folder is from a different recipe; "
                              "use another folder or remove it")
        self.results["started"] = earlier.get("started", self.results["started"])
        self.results["rows"] = [row for row in earlier.get("rows", []) if row.get("status") == "ok"]
        for row in self.results["rows"]:
            if self.round_ac is None and (row.get("power") or {}).get("ac") is not None:
                self.round_ac = row["power"]["ac"]

    def wanted_ac(self):
        require = self.power_rules.get("require", "any")
        if require == "mains":
            return True
        if require == "battery":
            return False
        if require == "same":
            return self.round_ac
        return None

    def check_power(self, row_name):
        """Waits for the power state the round needs, then checks the battery
        floor and the profile. Raises StopRun when the row cannot start."""
        rules = self.power_rules
        wait = float(rules.get("wait", 0))
        deadline = time.monotonic() + wait
        while True:
            power = read_power()
            wanted = self.wanted_ac()
            if wanted is None or power["ac"] is None or power["ac"] == wanted:
                break
            if time.monotonic() >= deadline:
                state = "mains" if wanted else "battery"
                raise StopRun(f"before {row_name} the machine was not on {state}"
                              + (f" after waiting {wait:.0f} s" if wait else ""))
            self.echo(f"  waiting for {'mains' if wanted else 'battery'} power before {row_name}")
            time.sleep(min(5.0, max(0.1, deadline - time.monotonic())))
        floor = rules.get("battery_floor")
        if floor is not None and power["ac"] is False and power["battery"] is not None:
            need = float(floor) + float(rules.get("battery_per_launch", 0))
            if power["battery"] < need:
                raise StopRun(f"before {row_name} the battery was at {power['battery']}%, under the "
                              f"{need:.0f}% a launch needs to stay above the {floor}% floor")
        profile = read_profile()
        if rules.get("profile") and profile is not None and profile != rules["profile"]:
            raise StopRun(f"before {row_name} the power profile was {profile}, not {rules['profile']}")
        if self.round_ac is None:
            self.round_ac = power["ac"]
        return power, profile

    def wait_for_load(self):
        """Waits while another process is busy; returns what the row notes."""
        limit = self.load_rules.get("max_cpu_percent")
        if limit is None:
            return {"waited": 0, "top": []}
        deadline = time.monotonic() + float(self.load_rules.get("wait", 600))
        started = time.monotonic()
        window = float(self.load_rules.get("window", 2.0))
        while True:
            top = sample_load(window)[:5]
            if not top or top[0][1] <= float(limit) or time.monotonic() >= deadline:
                waited = time.monotonic() - started - window
                return {"waited": round(max(0.0, waited), 1), "top": [list(item) for item in top],
                        "over": bool(top and top[0][1] > float(limit))}
            self.echo(f"  waiting: {top[0][0]} uses {top[0][1]:.0f}% of a core")
            time.sleep(float(self.load_rules.get("every", 3.0)))

    def command(self, row_name, variant):
        game = self.recipe["game"]
        spec = self.recipe["variants"].get(variant) or {}
        row_dir = self.out / row_name
        names = {**self.names, "row": row_name, "variant": variant, "out": str(self.out), "home": ""}
        env = dict(os.environ)
        for pattern in game.get("clear_env", []):
            for name in [name for name in env if fnmatch.fnmatchcase(name, pattern)]:
                del env[name]
        # A box (an error, a question) would wait for a click nobody makes; a
        # recipe's env may still set KISAK_NO_DIALOGS to 0.
        env.setdefault("KISAK_NO_DIALOGS", "1")
        binary = expand(spec.get("binary") or game["binary"], names)
        command = [expand(str(part), names) for part in game.get("wrap", [])] + [binary]
        if game.get("home_template"):
            home = row_dir / "home"
            if home.exists():
                shutil.rmtree(home)
            shutil.copytree(expand(game["home_template"], names), home, symlinks=True)
            names["home"] = str(home)
            xdg = row_dir / "xdg"
            xdg.mkdir(parents=True, exist_ok=True)
            env["XDG_DATA_HOME"] = str(xdg)
            command += ["+set", "fs_homepath", str(home)]
        for name, value in list(game.get("env", {}).items()) + list(spec.get("env", {}).items()):
            env[name] = expand(str(value), names)
        command += [expand(str(part), names) for part in list(game.get("args", [])) + list(spec.get("args", []))
                    + list(game.get("args_after", []))]
        cwd = expand(game["cwd"], names) if game.get("cwd") else str(row_dir)
        return command, env, cwd, names, binary

    def run_tool(self, step, names, row):
        """A tool beside the game or after it; its last lines go into the row."""
        argv = [expand(str(part), names) for part in step["run"]]
        name = step.get("name") or Path(argv[0]).name
        try:
            done = subprocess.run(argv, capture_output=True, text=True, errors="replace",
                                  timeout=float(step.get("timeout", 600)), cwd=str(self.out))
        except subprocess.TimeoutExpired:
            return False, f"{name} did not finish in {step.get('timeout', 600)} s"
        except OSError as error:
            return False, f"{name}: {error}"
        keep = int(step.get("keep_lines", 20))
        output = (done.stdout + done.stderr).splitlines()
        row.setdefault("outputs", {})[name] = output[-keep:] if keep else []
        return done.returncode == 0, f"{name} exit {done.returncode}"

    def play(self, game, row, names):
        """The scene. Returns False when a step that is not optional failed."""
        for step in self.recipe["scene"]:
            ok, detail, kind = True, "", next(key for key in step if key in STEP_KINDS)
            if kind == "wait_quiet":
                begun = time.monotonic()
                ok = game.wait_quiet(float(step["wait_quiet"]), float(step.get("timeout", 300)),
                                     float(step.get("min", 0)))
                detail = f"{time.monotonic() - begun:.1f} s"
            elif kind == "send":
                lines = step["send"] if isinstance(step["send"], list) else [step["send"]]
                for index, line in enumerate(lines):
                    if index:
                        game.wait_for(None, float(step.get("gap", 0.3)))
                    ok = ok and game.send(expand(line, names))
                detail = expand(lines[0], names) + (f" and {len(lines) - 1} more" if len(lines) > 1 else "")
            elif kind == "expect":
                match = game.wait_for(re.compile(step["expect"]), float(step.get("timeout", 30)))
                ok = match is not None
                detail = step["expect"]
                if match:
                    found = {key: value for key, value in match.groupdict().items() if value is not None}
                    names.update(found)
                    row["values"].update(found)
            elif kind == "poll":
                ok, detail = self.poll(game, row, names, step)
            elif kind == "sleep":
                game.wait_for(None, float(step["sleep"]))
                ok = game.alive()
            elif kind == "framelog":
                ok, detail = self.capture(game, row, names, step)
            elif kind == "run":
                watch = (PowerWatch(float(self.power_rules.get("watch_every", 1.0)))
                         if step.get("watch_power", True) else None)
                ok, detail = self.run_tool(step, names, row)
                if watch and watch.stop():
                    row["power"]["changed"] = True
            if not ok and not game.alive():
                detail = (detail + "; " if detail else "") + "the game ended"
            row["steps"].append([kind, ok, detail])
            self.echo(f"  {'ok ' if ok else 'BAD'} {row['name']}: {kind}" + (f" ({detail})" if detail else ""))
            if not ok and not step.get("optional"):
                return False
        return True

    def poll(self, game, row, names, step):
        pattern = re.compile(step["count"])
        wanted = int(step.get("at_least", 1))
        every = float(step.get("every", 2.0))
        deadline = time.monotonic() + float(step.get("timeout", 60))
        name = step.get("name", "count")
        count = 0
        while game.alive():
            if not game.send(expand(step["poll"], names)):
                break
            count = game.count_for(pattern, every)
            row["values"][name] = count
            if count >= wanted:
                return True, f"{name} {count}"
            if time.monotonic() >= deadline:
                break
        return False, f"{name} {count}, wanted {wanted}"

    def capture(self, game, row, names, step):
        seconds = float(step["framelog"])
        label = expand(str(step.get("label", "{row}")), names)
        wanted = self.wanted_ac()
        if wanted is not None and read_power()["ac"] not in (None, wanted):
            row["power"]["changed"] = True
            return False, "the power state changed before the capture"
        limit = self.load_rules.get("max_cpu_percent")
        if limit is not None:
            # The wait before the launch says nothing about now: the load, the
            # spawn and the bots took a minute or more.
            window = min(1.0, float(self.load_rules.get("window", 2.0)))
            began = time.monotonic()
            deadline = began + min(float(self.load_rules.get("wait", 600)),
                                   float(self.load_rules.get("wait_at_capture", 120)))
            while True:
                top = sample_load(window, exclude=(game.process.pid,))[:5]
                if not top or top[0][1] <= float(limit) or time.monotonic() >= deadline or not game.alive():
                    break
                self.echo(f"  waiting before the capture: {top[0][0]} uses {top[0][1]:.0f}% of a core")
                time.sleep(float(self.load_rules.get("every", 3.0)))
            row["load"]["at_capture"] = [list(item) for item in top]
            waited = time.monotonic() - began - window
            if waited >= 1.0:
                row["load"]["waited_at_capture"] = round(waited, 1)
        beside = None
        if step.get("with"):
            argv = [expand(str(part), names) for part in step["with"]["run"]]
            try:
                beside = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                          errors="replace", cwd=str(self.out))
            except OSError as error:
                return False, f"{argv[0]}: {error}"
        watch = PowerWatch(float(self.power_rules.get("watch_every", 1.0)))
        during, sampler = [], None
        if limit is not None:
            # The other processes over the capture itself (a tool that rides
            # along with the row is part of the row).
            skip = (game.process.pid,) + ((beside.pid,) if beside else ())
            sampler = threading.Thread(target=lambda: during.extend(sample_load(seconds, exclude=skip)[:5]),
                                       daemon=True)
            sampler.start()
        if not game.send(f"framelog {seconds:g} {label}"):
            watch.stop()
            if beside:
                beside.kill()
                beside.communicate()
            return False, "the game takes no input"
        match = None
        deadline = time.monotonic() + seconds + float(step.get("slack", 30))
        while time.monotonic() < deadline:
            found = game.wait_for(FRAME_LOG, deadline - time.monotonic())
            if found is None:
                break
            if (found.group("label") or "") == label:
                match = found
                break
        if watch.stop():
            row["power"]["changed"] = True
        if sampler:
            sampler.join(seconds + 10)
            row["load"]["during_capture"] = [list(item) for item in during]
            if during and during[0][1] > float(limit):
                row["load"]["busy"] = f"{during[0][0]} {during[0][1]:.0f}%"
        beside_note = ""
        if beside:
            name = step["with"].get("name") or Path(beside.args[0]).name
            try:
                output, _ = beside.communicate(timeout=float(step["with"].get("timeout", 60)))
            except subprocess.TimeoutExpired:
                beside.kill()
                output, _ = beside.communicate()
                beside_note = f"; {name} had to be stopped"
            keep = int(step["with"].get("keep_lines", 20))
            row.setdefault("outputs", {})[name] = output.splitlines()[-keep:] if keep else []
            if beside.returncode and not beside_note:
                beside_note = f"; {name} exit {beside.returncode}"
        if not match:
            return False, (f"no frame log line for {label} within "
                           f"{seconds + float(step.get('slack', 30)):.0f} s" + beside_note)
        summary = run_report.frame_summary(match)
        row["framelog"].append(summary)
        if match.group("why"):
            return False, f"the recording ended early ({match.group('why')})" + beside_note
        if beside_note:
            return False, f"{summary['fps']:.1f} fps" + beside_note
        return True, f"{summary['fps']:.1f} fps"

    def row(self, row_name, variant):
        others = running_names(self.recipe.get("stop_if_running", []))
        if others:
            raise StopRun(f"before {row_name}, {', '.join(others)} was running")
        power, profile = self.check_power(row_name)
        load = self.wait_for_load()
        row = {"name": row_name, "variant": variant, "status": "ok", "started": now_utc(), "seconds": 0.0,
               "exit": None, "power": {"ac": power["ac"], "battery_start": power["battery"],
                                       "battery_end": None, "profile": profile, "changed": False},
               "load": load, "framelog": [], "values": {}, "lines": {}, "counts": {}, "guard": "",
               "steps": [], "tail": []}
        if self.recipe["variants"].get(variant, {}).get("note"):
            row["note"] = self.recipe["variants"][variant]["note"]
        slept_before = slept_seconds()
        command, env, cwd, names, binary = self.command(row_name, variant)
        Path(cwd).mkdir(parents=True, exist_ok=True)
        row["binary"] = binary_identity(binary)
        leftovers = running_copies(binary)
        if leftovers:
            row["status"] = f"failed: {len(leftovers)} process(es) of this binary were still running"
            return row
        if self.guard and self.baseline is None:
            self.baseline = snapshot(self.guard)
        begun = time.monotonic()
        try:
            game = Game(command, cwd, env, self.out / f"{row_name}.log")
        except OSError as error:
            row["status"] = f"failed: the game did not start ({error})"
            return row
        played = False
        try:
            played = self.play(game, row, names)
        finally:
            status = game.finish(float(self.recipe["game"].get("quit_timeout", 60)))
        row["exit"] = status
        row["seconds"] = round(time.monotonic() - begun, 1)
        row["power"]["battery_end"] = read_power()["battery"]
        for step in self.recipe.get("after", []):
            ok, detail = self.run_tool(step, names, row)
            row["steps"].append(["after", ok, detail])
            if not ok and not step.get("optional"):
                played = False
        for pattern in self.recipe.get("lines", []):
            compiled = re.compile(pattern)
            row["lines"][pattern] = [line for line in game.lines if compiled.search(line)]
        counts = dict(STANDARD_COUNTS)
        counts.update({name: re.compile(pattern) for name, pattern in self.recipe.get("counts", {}).items()})
        row["counts"] = {name: sum(1 for line in game.lines if pattern.search(line))
                         for name, pattern in counts.items()}
        if self.guard:
            row["guard"] = compare_snapshots(self.baseline, snapshot(self.guard))
        slept = slept_seconds() - slept_before
        if slept >= 1.0:
            row["power"]["slept"] = round(slept, 1)
        if row["power"].get("slept"):
            # First, like the next: a row spoiled by the power is run again.
            row["status"] = f"flagged: the power: the machine slept {row['power']['slept']:g} s during the row"
        elif row["power"]["changed"]:
            row["status"] = "flagged: the power state changed during the capture"
        elif row["load"].get("busy"):
            row["status"] = f"flagged: other load during the capture ({row['load']['busy']} of a core)"
        elif not played:
            failed = next((step for step in row["steps"] if not step[1]), None)
            row["status"] = "failed: " + (f"{failed[0]} ({failed[2]})" if failed else "the scene did not finish")
        elif status is None:
            row["status"] = "failed: killed after the quit timed out"
        elif status != 0:
            row["status"] = f"failed: exit {status}"
        elif self.guard and row["guard"] != "ok":
            row["status"] = "flagged: the guard saw a change"
        if row["status"] != "ok":
            row["tail"] = game.lines[-TAIL_LINES:]
        return row

    def run(self, only=None, done=()):
        retries = int(self.power_rules.get("retries", 0))
        try:
            for row_name, variant in row_names(self.recipe["rows"]):
                if only and row_name not in only and variant not in only:
                    continue
                if row_name in done:
                    self.echo(f"{row_name}: already done")
                    continue
                attempt = 0
                while True:
                    if (self.out / "PAUSE").exists():
                        raise StopRun(f"a PAUSE file stopped the run before {row_name}")
                    self.echo(f"{row_name}: launching")
                    row = self.row(row_name, variant)
                    self.echo(f"{row_name}: {row['status']}")
                    spoiled = row["status"].startswith(("flagged: the power", "flagged: other load"))
                    if spoiled and attempt < retries:
                        attempt += 1
                        row["name"] = f"{row_name}.spoiled{attempt}"
                        self.results["rows"].append(row)
                        self.save()
                        continue
                    self.results["rows"].append(row)
                    self.save()
                    break
            bad = [row["name"] for row in self.results["rows"]
                   if row["status"] != "ok" and ".spoiled" not in row["name"]]
            self.results["status"] = "complete" if not bad else "complete, with rows not ok: " + ", ".join(bad)
        except StopRun as stop:
            self.results["status"] = f"stopped: {stop}"
        self.save()
        return self.results["status"] == "complete"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("recipe", help="the run's recipe (JSON)")
    parser.add_argument("--out", required=True, help="output folder (on the machine, outside the repository)")
    parser.add_argument("--rows", help="only these rows or variants, comma separated (A-1,D)")
    parser.add_argument("--resume", action="store_true", help="skip rows that results.json already has as ok")
    parser.add_argument("--check", action="store_true", help="read the recipe, print the plan and stop")
    parser.add_argument("--names", help="this machine's names file (JSON), for the {names} a recipe uses")
    parser.add_argument("--set", action="append", default=[], metavar="NAME=VALUE",
                        help="one name for this run; may be given several times")
    args = parser.parse_args(argv)

    try:
        recipe = load_recipe(args.recipe)
        recipe_text = Path(args.recipe).read_text(encoding="utf-8")
        machine_names = load_names(args.names, args.set)
        missing = missing_names(recipe, machine_names)
        if missing:
            raise RecipeError(f"{args.recipe} uses {', '.join('{' + name + '}' for name in missing)}, which neither "
                              "the names file nor --set gives")
    except RecipeError as error:
        print(f"perf_run.py: {error}", file=sys.stderr)
        return 2
    names = row_names(recipe["rows"])
    if args.check:
        print(f"{recipe['id']}: {len(names)} launches: " + " ".join(name for name, _ in names))
        power = read_power()
        print(f"power now: ac {power['ac']}, battery {power['battery']}, profile {read_profile()}")
        binaries = []
        for _, variant in names:
            spec = recipe["variants"].get(variant) or {}
            binary = expand(spec.get("binary") or recipe["game"]["binary"], machine_names)
            if binary not in binaries:
                binaries.append(binary)
        for binary in binaries:
            identity = binary_identity(binary)
            print(f"game: {binary} " + (f"(sha256 {identity['sha256']}, built {identity['modified']})"
                                        if identity else "(NOT FOUND)"))
        return 0 if all(os.path.exists(binary) for binary in binaries) else 1

    out = Path(os.path.expanduser(args.out))
    out.mkdir(parents=True, exist_ok=True)
    run = Run(recipe, recipe_text, out, names=machine_names)
    done = ()
    earlier = out / "results.json"
    if earlier.exists():
        if not args.resume:
            print(f"perf_run.py: {earlier} exists; pass --resume to go on from it, or use another folder",
                  file=sys.stderr)
            return 2
        try:
            run.adopt(run_report.load(earlier))
        except (OSError, ValueError, RecipeError) as error:
            print(f"perf_run.py: {error}", file=sys.stderr)
            return 2
        done = tuple(row["name"] for row in run.results["rows"])
    only = set(args.rows.split(",")) if args.rows else None
    complete = run.run(only, done)
    print(f"{recipe['id']}: {run.results['status']}")
    print(f"results: {out / 'results.json'}")
    print(f"report:  {out / 'report.md'}")
    return 0 if complete else 1


if __name__ == "__main__":
    sys.exit(main())
