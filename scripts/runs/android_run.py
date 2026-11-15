#!/usr/bin/env python3
"""Run the Android app on a device from one recipe, unattended.

Not a CI check: it needs a device with the game's files on it, and runs on
the test machine the device is plugged into (docs/runs.md, "Android runs from
one recipe: android_run.py"). Written for the Windows PC and the Retroid
Pocket 6, where every Android run used to be typed by hand: build the APK in
WSL, install it, start it, time the screen captures by eye, save logcat.
Standard library only, like pc_run.py, whose recipe checks, timed commands,
variants, picture numbers and report it reuses.

    android_run.py recipe.json --out <folder>            run it
    android_run.py recipe.json --out <folder> --dry-run  print the steps only
    android_run.py --queue <folder> --out-root <folder>  run every recipe queued
    android_run.py --queue <folder> --out-root <folder> --watch 60
                                                         ... and keep looking

One run is, in order:

1. ``build`` (optional): check out ``ref`` in the Android source tree, build
   the APK with scripts/platform/android/build_apk.py and copy it to
   ``<out>/app.apk``. With ``"wsl": true`` the build runs in WSL (``wsl.exe``)
   while the runner and adb run on Windows; the source tree must have no
   changes of its own, or the runner leaves it alone and stops. Or ``apk``
   names an APK already built (a CI artifact).
2. ``install`` (default true when there is an APK): ``adb install -r`` only,
   which keeps the app's data (game files, profile, config). Never an
   uninstall. The report gives the package's first-install time before and
   after, which shows the data was kept.
3. Each launch: force-stop the app, start it with ``am start`` and the
   launch's arguments in the ``args`` extra (KisakActivity splits it at
   spaces and puts it after its own ``fs_basepath``/``fs_homepath``), read
   logcat as it comes, run the host steps (screen captures, taps, keys) timed
   from the moment the app started, the menu came up or the map came up (the
   client's "run: the menu is up" and "run: the map is up" lines), and stop
   when the game quits, crashes, or runs out of time. Then pull the
   screenshots the game took (``screenshot <name>`` in a timed command writes
   ``home/main/screenshots/<name>.tga`` in the app's folder). Each host step
   runs at its own moment, whatever the others wait for. The game is
   quitting once it sends a timed ``quit`` (``run: sending "quit"``) or its
   ``run_quitAfter*`` timer says ``run: quitting as asked``. A crash after
   that is ``crash_at_quit``, not ``crash``: the launch did everything it
   asked first.

The launch's timed commands (``at_menu``, ``at_map``, ``variants``,
``quit_after_map``) are pc_run.py's, with the same checks: the client reads
them from ``run_atMenu``/``run_atMap`` (docs/runs.md, "Runs that play
themselves"). On Android they cannot be quoted: KisakActivity splits the
extra at spaces and the game would quote each piece again, so the runner
gives ``+set run_atMap 5 setviewpos 1 2 3 | 12 quit`` unquoted, which ``set``
joins back together. A '"' anywhere in the arguments is refused. The game's
whole command line (KisakActivity's own arguments too) must fit in 1023
bytes (``sys_cmdline``), or the game does not start: the recipe check
refuses a launch whose arguments would not fit.

The recipe (JSON)::

    {
      "format": "kisak-android-run/1",
      "id": "android-run-6",
      "title": "Pocket 6: Crash, menu and map, with screen captures",
      "adb": "C:/kisak/platform-tools/adb.exe",
      "device": {"serial": "", "package": "org.kisakcod.game", "activity": ".KisakActivity"},
      "build": {"wsl": true, "source": "~/kisak-android-src", "ref": "origin/claude/project-thread-g6j4a6",
                "config": "RelWithDebInfo", "work": "~/kisak-android", "ndk": "~/android/android-ndk-r28c",
                "sdk": "~/android/sdk", "gradle": "~/android/gradle-8.14.3/bin/gradle", "timeout": 1800},
      "args": ["+set", "com_maxfps", "0"],
      "timeout": 300,
      "launches": [
        {"name": "menu", "at_menu": ["20 quit"],
         "host": [{"after": "menu", "at": 10, "do": "screencap", "name": "menu-10"}]},
        {"name": "crash", "args": ["+devmap", "mp_crash"],
         "at_map": ["3 cmd mr {serverid} 8 autoassign", "5 cmd mr {serverid} 15 assault_mp,0",
                    "25 framelog 10 crash", "37 screenshot crash-37", "40 quit"],
         "host": [{"after": "map", "at": 30, "do": "screencap", "name": "crash-30"},
                  {"after": "map", "at": 32, "do": "key", "code": "KEYCODE_BUTTON_R1", "source": "gamepad"}]}
      ],
      "lines": ["vulkan:", "swapchain", "error", "fatal"],
      "pictures": {"regions": {"centre": [860, 440, 1060, 640]}, "levels": [2, 16]}
    }

Host steps (``host``): ``after`` is ``start``, ``menu`` or ``map``, ``at`` the
seconds after it, ``do`` one of ``screencap`` (``name``: the PNG's name, like
a screenshot's), ``tap`` (``x``, ``y``), ``swipe`` (``x1``, ``y1``, ``x2``,
``y2``, optional ``ms``), ``key`` (``code``: ``KEYCODE_...`` or a number,
optional ``source``: gamepad, keyboard, dpad, joystick, touchscreen) or
``text`` (``text``: letters, digits, ``_ . -``). Nothing else reaches the
device: no shell command of the recipe's own. A screen capture is the whole
panel as the compositor shows it (1920x1080 on the Pocket 6) and works
whatever the renderer does; the game's own ``screenshot`` is the game's back
buffer. Both go through the picture numbers (``pictures``, pc_run.py's).
Host step times are the PC's clock against the logcat line, so they are good
to a few tenths of a second; for anything exact use a timed command.

Each launch waits (``wait_for_app``, default 2 minutes) while the app is already running, since
someone may be playing it, and then gives up with ``blocked`` unless the
recipe says ``"stop_running_app": true``. A sleeping screen is woken
(``KEYCODE_WAKEUP``) before a launch. After the game says it is quitting
the runner gives the app 10 s to end and then force-stops it, as SDL's
activity can outlive the game.

On the PC's own emulator instead of a device: ``emulator`` starts the
Android emulator with a virtual device (``avd``, made once with avdmanager,
an x86_64 Android 13 image for an x86_64 build of the app) on ``port``
(default 5554, so adb calls it ``emulator-5554``), with ``-gpu host``
(``gpu``: gfxstream Vulkan on the PC's own GPU), no window unless
``"window": true``, no audio, and no snapshot saved, waits until it has
booted (``boot_timeout``, default 300 s), and shuts it down after the run
(``adb emu kill``) unless ``"keep_running": true``. An emulator that already
answers on the port is used as found and left running. ``game_files`` copies
the game's files from this machine (``from``, your own install) into the
app's folder with ``adb push`` when ``check`` (a file such as
``main/iw_00.iwd``) is not there yet, which is once per virtual device.
``emulator.env`` sets the emulator's environment, such as ``ANDROID_AVD_HOME``
for virtual devices kept off the system drive::

      "emulator": {"exe": "E:/Android/Sdk/emulator/emulator.exe", "avd": "kisak-a13-x64",
                   "env": {"ANDROID_AVD_HOME": "E:/Android/avd"}},
      "game_files": {"from": "D:/Games/Call of Duty 4", "check": "main/iw_00.iwd"},
      "build": {..., "args": ["--abi", "x86_64"]},

``build.args`` go to build_apk.py as they are, and ``build.apk_name`` names
the APK it writes when that is not ``KisakCOD-<config>.apk``. The build runs
before the emulator starts, so the emulator does not sit through it.

The queue: ``--queue <folder>`` runs every ``*.json`` recipe in the folder,
by name, whose ``<out-root>/<id>/results.json`` does not exist yet, one at a
time (``<id>`` is the recipe's id, else the file's name). A file named
``PAUSE`` in the queue folder stops it before the next recipe. ``--watch N``
looks again every N seconds until stopped. A lock file in the out-root keeps
a second runner off the same device.

It writes ``<out>/results.json`` (checksummed like pc_run.py's),
``<out>/report.md``, ``<out>/build.log``, and per launch
``<out>/<launch>/logcat-all.txt`` (everything the device logged from the
start), ``logcat.txt`` (the game's tags, its process and crash lines), the
screen captures and the game's screenshots, and ``<out>/pictures/``. Game
pictures and logs stay on the machine; the report and results.json are what
crosses.

Exit status: 0 when the build, the install and every launch went as asked
(the game quit by itself, every screenshot and screen capture is there,
every host step ran), 1 otherwise, 2 on a bad recipe or a queue that is
locked.
"""

import argparse
import hashlib
import json
import os
import queue
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pc_run  # noqa: E402
from pc_run import RecipeError  # noqa: E402

FORMAT = "kisak-android-run/1"
DEFAULT_PACKAGE = "org.kisakcod.game"
DEFAULT_ACTIVITY = ".KisakActivity"
# src/linux/main.cpp builds the game's command line in sys_cmdline[1024].
COMMAND_LINE_BYTES = pc_run.COMMAND_LINE_BYTES
# What KisakActivity.getArguments puts before the extra's arguments, with the
# path getExternalFilesDir gives on a device's own storage.
APP_FOLDER = "/storage/emulated/0/Android/data/{package}/files"
APP_ARGUMENTS = ["+set", "fs_basepath", "{folder}", "+set", "fs_homepath", "{folder}/home",
                 "+set", "r_fullscreen", "1"]
# The same folder as adb's shell sees it, for pulling screenshots.
DEVICE_HOME = "/sdcard/Android/data/{package}/files/home"
GAME_PROCESS = ":game"
DEFAULT_TAGS = ["KisakCOD", "SDL", "AndroidRuntime", "DEBUG", "libc"]
LOGCAT_LINE = re.compile(r"^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+(\d+)\s+(\d+)\s+([VDIWEFA])\s+(.*?)\s*: ?(.*)$")
EVENT_LINES = {"menu": "run: the menu is up", "map": "run: the map is up", "quit": "run: quitting as asked"}
# A "quit" in a timed list is only sent ("run: sending "quit"", by
# src/client/cl_run_plan.h); "run: quitting as asked" comes from the
# run_quitAfter* timers alone (item 351).
SENT_LINE = re.compile(r'^run: sending "(.*)"$')


def says_quit(message):
    """Whether a game line means it is quitting as the launch asked."""
    message = message.strip()
    if message == EVENT_LINES["quit"]:
        return True
    sent = SENT_LINE.match(message)
    return bool(sent) and any(part.split()[:1] == ["quit"] for part in sent.group(1).split(";"))
CRASH = re.compile(r"Fatal signal|FATAL EXCEPTION|ANR in ")
HOST_AFTER = ("start", "menu", "map")
KEY_SOURCES = ("gamepad", "keyboard", "dpad", "joystick", "touchscreen")
KEY_CODE = re.compile(r"^(KEYCODE_[A-Z0-9_]+|\d{1,3})$")
SAFE_TEXT = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
NAME = re.compile(r"^[A-Za-z0-9_.-]+$")
WAIT_FOR_RUNNING_APP = 120
QUIT_GRACE = 10
NEVER_STARTED = 30
POLL = 0.1
PID_EVERY = 1.0
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
DEFAULT_EMULATOR_PORT = 5554
DEFAULT_BOOT_TIMEOUT = 300
SHARED_STORAGE = "/sdcard/Android/data"  # there once shared storage is mounted
MKDIR_BATCH = 20
EMULATOR_ARGS = ["-no-snapshot-save", "-no-boot-anim", "-no-audio"]


# ---------------------------------------------------------------- the recipe

def device(recipe):
    value = recipe.get("device") or {}
    serial = value.get("serial") or ""
    if recipe.get("emulator"):
        serial = f"emulator-{emulator_port(recipe)}"
    return {"serial": serial, "package": value.get("package") or DEFAULT_PACKAGE,
            "activity": value.get("activity") or DEFAULT_ACTIVITY}


def emulator_port(recipe):
    return int((recipe.get("emulator") or {}).get("port", DEFAULT_EMULATOR_PORT))


def launch_arguments(recipe, launch):
    """The engine arguments the launch's extra carries, in order: the
    recipe's, the run settings, then the launch's own +devmap and the like."""
    args = [str(arg) for arg in recipe.get("args") or []]
    at_menu, at_map = pc_run.launch_commands(launch, "at_menu"), pc_run.launch_commands(launch, "at_map")
    if at_menu:
        args += ["+set", "run_atMenu", pc_run.timed_list(at_menu)]
    if at_map:
        args += ["+set", "run_atMap", pc_run.timed_list(at_map)]
        if "quit_after_map" not in launch:
            last = max(float(pc_run.TIMED.match(command).group(1)) for command in at_map)
            args += ["+set", "run_quitAfterMap", str(int(last) + pc_run.SAFETY_AFTER_LAST)]
    if launch.get("quit_after_map"):
        args += ["+set", "run_quitAfterMap", str(int(launch["quit_after_map"]))]
    return args + [str(arg) for arg in launch.get("args") or []]


def extra_text(recipe, launch):
    return " ".join(" ".join(launch_arguments(recipe, launch)).split())


def game_command_line(recipe, launch):
    """The command line the game builds (src/linux/main.cpp BuildCommandLine):
    KisakActivity's arguments and the extra split at spaces, joined by spaces,
    a piece quoted when it holds a space, a quote or '//'."""
    folder = APP_FOLDER.format(package=device(recipe)["package"])
    pieces = [arg.replace("{folder}", folder) for arg in APP_ARGUMENTS] + extra_text(recipe, launch).split()
    quoted = []
    for piece in pieces:
        if re.search(r'[ \t"]', piece) or "//" in piece or "/*" in piece:
            piece = '"' + piece.replace('"', '\\"') + '"'
        quoted.append(piece)
    return " ".join(quoted)


def check_host_steps(launch, shots):
    name = launch["name"]
    for step in launch.get("host") or []:
        where = f"{name}: host step {step!r}"
        if step.get("after", "start") not in HOST_AFTER:
            raise RecipeError(f"{where}: after is one of {', '.join(HOST_AFTER)}")
        at = step.get("at")
        if not isinstance(at, (int, float)) or at < 0:
            raise RecipeError(f"{where}: at is a number of seconds")
        kind = step.get("do")
        if kind == "screencap":
            shot = step.get("name", "")
            if not NAME.match(shot or ""):
                raise RecipeError(f"{where}: a screen capture needs a name of letters, digits, _ . -")
            if shot in shots:
                raise RecipeError(f"screenshot {shot!r} is named twice; the second would overwrite the first")
            shots.add(shot)
        elif kind == "tap":
            numbers(where, step, ("x", "y"))
        elif kind == "swipe":
            numbers(where, step, ("x1", "y1", "x2", "y2") + (("ms",) if "ms" in step else ()))
        elif kind == "key":
            if not KEY_CODE.match(str(step.get("code", ""))):
                raise RecipeError(f"{where}: code is KEYCODE_<NAME> or a number")
            if step.get("source", "keyboard") not in KEY_SOURCES:
                raise RecipeError(f"{where}: source is one of {', '.join(KEY_SOURCES)}")
        elif kind == "text":
            if not SAFE_TEXT.match(str(step.get("text", ""))):
                raise RecipeError(f"{where}: text is 1 to 64 letters, digits, _ . -")
        else:
            raise RecipeError(f"{where}: do is screencap, tap, swipe, key or text")


def numbers(where, step, keys):
    for key in keys:
        if not isinstance(step.get(key), int) or step[key] < 0:
            raise RecipeError(f"{where}: {key} is a whole number of pixels (or ms)")


def load_recipe(path):
    try:
        recipe = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RecipeError(f"cannot read {path}: {error}")
    if recipe.get("format") != FORMAT:
        raise RecipeError(f"format is {recipe.get('format')!r}, expected {FORMAT!r}")
    if not recipe.get("adb"):
        raise RecipeError("adb is missing (the adb program, or a list: program and its first arguments)")
    package = device(recipe)["package"]
    if not re.fullmatch(r"org\.kisakcod\.[a-z0-9_.]+", package):
        raise RecipeError(f"package {package!r}: only the game's own (org.kisakcod.*) is started or installed")
    if not re.fullmatch(r"\.?[A-Za-z0-9_.]+", device(recipe)["activity"]):
        raise RecipeError("device.activity is a class name such as .KisakActivity")
    emulator = recipe.get("emulator")
    if emulator:
        if not emulator.get("exe") or not NAME.match(str(emulator.get("avd", ""))):
            raise RecipeError("emulator needs exe (emulator.exe) and avd (the virtual device's name)")
        port = emulator.get("port", DEFAULT_EMULATOR_PORT)
        if not isinstance(port, int) or port % 2 or not 5554 <= port <= 5682:
            raise RecipeError("emulator.port is an even number from 5554 to 5682")
        if (recipe.get("device") or {}).get("serial"):
            raise RecipeError("give device.serial or emulator, not both: the emulator is emulator-<port>")
        if not isinstance(emulator.get("env", {}), dict):
            raise RecipeError("emulator.env is an object of environment variables")
    files = recipe.get("game_files")
    if files and (not files.get("from") or not re.fullmatch(r"[A-Za-z0-9_./-]+", str(files.get("check", "")))):
        raise RecipeError("game_files needs from (the game folder on this machine) and check (a file in it, "
                          "such as main/iw_00.iwd, whose absence on the device means a copy is needed)")
    if recipe.get("build") and recipe.get("apk"):
        raise RecipeError("give build or apk, not both")
    build = recipe.get("build")
    if build:
        for key in ("source", "work", "ndk", "sdk", "gradle"):
            if not build.get(key):
                raise RecipeError(f"build.{key} is missing")
        if not isinstance(build.get("args", []), list) or not NAME.match(str(build.get("apk_name", "x.apk"))):
            raise RecipeError("build.args is a list of build_apk.py arguments; build.apk_name a file name")
        if build.get("config", "RelWithDebInfo") not in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
            raise RecipeError("build.config is a CMake configuration")
        ref = build.get("ref", "")
        if ref and not re.fullmatch(r"[A-Za-z0-9_./-]+", ref):
            raise RecipeError(f"build.ref {ref!r}: a branch, tag or commit")
    launches = recipe.get("launches") or []
    if not launches:
        raise RecipeError("no launches")
    names, shots = set(), set()
    for launch in launches:
        name = launch.get("name", "")
        if not NAME.match(name or ""):
            raise RecipeError(f"launch name {name!r}: letters, digits, _ . - only")
        if name in names:
            raise RecipeError(f"launch name {name!r} given twice")
        names.add(name)
        pc_run.check_variants(launch)
        for key in ("at_menu", "at_map"):
            commands = pc_run.launch_commands(launch, key)
            if len(commands) > 32:
                raise RecipeError(f"{name}: {key} has {len(commands)} commands; the client takes 32")
            for command in commands:
                if "|" in command or not pc_run.TIMED.match(command):
                    raise RecipeError(f"{name}: {key} entry {command!r} is not '<seconds> <command>' without '|'")
                if "+" in command or "\n" in command:
                    raise RecipeError(f"{name}: {key} entry {command!r} has a '+': the game splits its command line "
                                      "at every '+', so the whole list would be refused")
                if len(command.split(None, 1)[1]) >= 160:
                    raise RecipeError(f"{name}: {key} entry {command!r} is longer than the client takes")
        for arg in launch_arguments(recipe, launch):
            if '"' in arg or "\n" in arg:
                raise RecipeError(f"{name}: argument {arg!r} has a quote or a line break; on Android the "
                                  "arguments go unquoted (KisakActivity splits them at spaces)")
        line = game_command_line(recipe, launch)
        if len(line) > COMMAND_LINE_BYTES:
            raise RecipeError(f"{name}: the game's command line would be {len(line)} bytes with the app's own "
                              f"arguments; it takes {COMMAND_LINE_BYTES} and does not start otherwise. "
                              "Fewer or shorter timed commands, or fewer variants per launch")
        parts = line.count("+")
        if parts > pc_run.MAX_CONSOLE_LINES:
            raise RecipeError(f"{name}: the command line has {parts} '+' parts; the game reads the first "
                              f"{pc_run.MAX_CONSOLE_LINES} and drops the rest")
        for shot in pc_run.launch_shots(launch):
            if shot in shots:
                raise RecipeError(f"screenshot {shot!r} is named twice; the second would overwrite the first")
            shots.add(shot)
        check_host_steps(launch, shots)
    pictures = recipe.get("pictures") or {}
    for pair in pictures.get("compare", []):
        for side in ("a", "b"):
            if pair.get(side) not in shots:
                raise RecipeError(f"compare names {pair.get(side)!r}, which no launch takes")
    for name, box in (pictures.get("regions") or {}).items():
        if len(box) != 4 or not (box[0] < box[2] and box[1] < box[3]):
            raise RecipeError(f"region {name}: [x0, y0, x1, y1] with x0 < x1 and y0 < y1")
    return recipe


# ------------------------------------------------------------------ the device

class Adb:
    def __init__(self, recipe):
        program = recipe["adb"]
        self.base = list(program) if isinstance(program, list) else [program]
        serial = device(recipe)["serial"]
        if serial:
            self.base += ["-s", serial]

    def run(self, *args, timeout=60, binary=False):
        try:
            done = subprocess.run(self.base + list(args), capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, b"" if binary else "", f"adb {' '.join(args)}: no answer in {timeout} s"
        except OSError as error:
            return 127, b"" if binary else "", f"cannot run adb: {error}"
        out = done.stdout if binary else done.stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")
        return done.returncode, out, done.stderr.decode("utf-8", errors="replace").strip()

    def shell(self, command, timeout=60):
        return self.run("shell", command, timeout=timeout)

    def popen(self, *args):
        return subprocess.Popen(self.base + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def app_pids(adb, package, names=None):
    """The pids of the app's processes: the launcher's (the package's own
    name) and the game's (``<package>:game``, AndroidManifest.xml)."""
    names = names or [package, package + GAME_PROCESS]
    code, out, _ = adb.shell("pidof " + " ".join(names), timeout=20)
    return [int(word) for word in out.split() if word.isdigit()] if code == 0 else []


def game_pids(adb, package, seen):
    """The game's own process. The app runs its game activities in a
    process of their own (``android:process=":game"``), so the launcher's
    process stays up when the game ends; a build from before that has one
    process, the package's. ``seen`` remembers which this build has."""
    pids = app_pids(adb, package, [package + GAME_PROCESS])
    if pids:
        seen["game_process"] = True
        return pids
    return [] if seen.get("game_process") else app_pids(adb, package, [package])


def package_times(adb, package):
    code, out, _ = adb.shell(f"dumpsys package {package}", timeout=30)
    found = {}
    for key in ("firstInstallTime", "lastUpdateTime", "versionName", "versionCode"):
        match = re.search(rf"\b{key}=(.+)", out)
        if match:
            found[key] = match.group(1).strip()
    return found


def device_pictures(adb, package):
    """{device path: "mtime size"} of the TGAs under the app's home folder."""
    home = DEVICE_HOME.format(package=package)
    _, out, _ = adb.shell(f"find {home} -name '*.tga' -exec stat -c '%Y %s %n' {{}} + 2>/dev/null", timeout=60)
    found = {}
    for line in out.splitlines():
        parts = line.strip().split(" ", 2)
        if len(parts) == 3 and parts[2].endswith(".tga"):
            found[parts[2]] = f"{parts[0]} {parts[1]}"
    return found


def host_command(step):
    kind = step["do"]
    if kind == "tap":
        return f"input tap {step['x']} {step['y']}"
    if kind == "swipe":
        return f"input swipe {step['x1']} {step['y1']} {step['x2']} {step['y2']}" + \
            (f" {step['ms']}" if "ms" in step else "")
    if kind == "key":
        source = step.get("source")
        return f"input {source + ' ' if source else ''}keyevent {step['code']}"
    if kind == "text":
        return f"input text {step['text']}"
    raise ValueError(kind)


def start_command(recipe, launch):
    target = device(recipe)
    command = f"am start -W -n {target['package']}/{target['activity']}"
    extra = extra_text(recipe, launch)
    if extra:
        command += f" --es args {shlex.quote(extra)}"
    return command


# ---------------------------------------------------------------- the emulator

def emulator_argv(recipe):
    emulator = recipe["emulator"]
    program = emulator["exe"]
    argv = list(program) if isinstance(program, list) else [program]
    argv += ["-avd", emulator["avd"], "-port", str(emulator_port(recipe)), "-gpu", emulator.get("gpu", "host")]
    argv += EMULATOR_ARGS + ([] if emulator.get("window") else ["-no-window"])
    return argv + [str(arg) for arg in emulator.get("args") or []]


def booted(adb):
    code, state, _ = adb.run("get-state", timeout=20)
    if code != 0 or state.strip() != "device":
        return False
    _, value, _ = adb.shell("getprop sys.boot_completed", timeout=20)
    if value.strip() != "1":
        return False
    # Shared storage comes up after boot_completed: before it, the game
    # files' mkdir gets "mkdir: '/sdcard/Android': Permission denied" (PC
    # item 365, an emulator the runner had just booted).
    return adb.shell(f"ls -d {SHARED_STORAGE}", timeout=20)[0] == 0


class Emulator:
    """The emulator for a run: started here unless one already answers on its
    port (then it is left running afterwards, as found)."""

    def __init__(self, recipe, adb, out):
        self.recipe, self.adb, self.out = recipe, adb, out
        self.process = None
        self.result = {"avd": recipe["emulator"]["avd"], "serial": device(recipe)["serial"]}

    def start(self):
        if booted(self.adb):
            self.result["status"] = "already running"
            return True
        started = time.time()
        log = open(self.out / "emulator.log", "wb")
        try:
            env = dict(os.environ, **{key: str(value) for key, value in
                                      (self.recipe["emulator"].get("env") or {}).items()})
            self.process = subprocess.Popen(emulator_argv(self.recipe), stdout=log, stderr=subprocess.STDOUT, env=env)
        except OSError as error:
            self.result["status"] = f"cannot start: {error}"
            return False
        finally:
            log.close()
        limit = float(self.recipe["emulator"].get("boot_timeout", DEFAULT_BOOT_TIMEOUT))
        while time.time() - started < limit:
            if self.process.poll() is not None:
                self.result["status"] = f"exited {self.process.returncode} before it booted (emulator.log)"
                return False
            if booted(self.adb):
                self.result["status"] = "booted"
                self.result["boot_seconds"] = round(time.time() - started, 1)
                return True
            time.sleep(2)
        self.result["status"] = f"not booted after {limit:g} s (emulator.log)"
        return False

    def stop(self):
        if not self.process or self.recipe["emulator"].get("keep_running"):
            return
        self.adb.run("emu", "kill", timeout=30)
        try:
            self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=30)
        self.result["stopped"] = True


def run_game_files(recipe, adb):
    """Copy the game's files from this machine into the app's folder when the
    check file is not there (a new virtual device). The app makes its folder
    at its first start, so it is started and stopped once first."""
    files = recipe["game_files"]
    package = device(recipe)["package"]
    folder = f"/sdcard/Android/data/{package}/files"
    result = {"check": files["check"]}
    if adb.shell(f"ls {folder}/{files['check']}", timeout=30)[0] == 0:
        result["status"] = "already there"
        return result
    source = Path(files["from"])
    if not (source / files["check"]).is_file():
        result["status"] = f"failed: {files['check']} is not in {source}"
        return result
    started = time.time()
    if adb.shell(f"ls {folder}", timeout=30)[0] != 0:
        adb.shell(f"am start -W -n {package}/{device(recipe)['activity']}", timeout=60)
        time.sleep(5)
        adb.shell(f"am force-stop {package}")
    # adbd makes only one new folder level per file on some images (the
    # Android 13 x86_64 emulator image: "remote secure_mkdirs failed:
    # Operation not permitted"), while the shell's mkdir -p works, so the
    # whole folder tree is made first.
    folders = sorted({f"{folder}/{path.relative_to(source).as_posix()}"
                      for path in source.rglob("*") if path.is_dir()})
    for start in range(0, len(folders), MKDIR_BATCH):
        batch = " ".join(shlex.quote(path) for path in folders[start:start + MKDIR_BATCH])
        code, out, err = adb.shell(f"mkdir -p {batch}", timeout=60)
        if code != 0:
            result["status"] = f"failed: mkdir -p on the device: {(out + ' ' + err).strip()}"
            return result
    result["folders"] = len(folders)
    code, out, err = adb.run("push", f"{source}/.", f"{folder}/", timeout=float(files.get("timeout", 7200)))
    result["output"] = (out + "\n" + err).strip().splitlines()[-2:]
    ok = code == 0 and adb.shell(f"ls {folder}/{files['check']}", timeout=30)[0] == 0
    result["status"] = "copied" if ok else "failed"
    result["seconds"] = round(time.time() - started, 1)
    return result


# ------------------------------------------------------------------- the build

def bash_path(value):
    """A path for bash: '~/x' keeps its ~ working, everything else is quoted."""
    value = str(value)
    if value == "~":
        return '"$HOME"'
    if value.startswith("~/"):
        return '"$HOME"/' + shlex.quote(value[2:])
    return shlex.quote(value)


def build_script(build, target_windows=None, target=None):
    config = build.get("config", "RelWithDebInfo")
    lines = ["set -euo pipefail", f"cd {bash_path(build['source'])}"]
    if build.get("ref"):
        lines += ["if ! git diff --quiet || ! git diff --cached --quiet; then",
                  "  echo 'android_run: the source tree has changes of its own; leaving it as it is' >&2",
                  "  exit 3", "fi",
                  "git fetch -q origin",
                  f"git checkout -q --detach {shlex.quote(build['ref'])}"]
    lines += ['echo "android_run: commit $(git rev-parse HEAD)"',
              f"{build.get('python', 'python3')} scripts/platform/android/build_apk.py"
              f" --work {bash_path(build['work'])} --ndk {bash_path(build['ndk'])} --sdk {bash_path(build['sdk'])}"
              f" --gradle {bash_path(build['gradle'])} --config {config}"
              + "".join(f" {shlex.quote(str(arg))}" for arg in build.get("args") or [])]
    apk = f"{bash_path(build['work'])}/{shlex.quote(build.get('apk_name') or f'KisakCOD-{config}.apk')}"
    if target_windows:
        lines.append(f'cp {apk} "$(wslpath -u {shlex.quote(str(target_windows))})"')
    else:
        lines.append(f"cp {apk} {shlex.quote(str(target))}")
    return "\n".join(lines) + "\n"


def build_argv(build, out):
    target = (out / "app.apk").resolve()
    if build.get("wsl"):
        wsl = ["wsl.exe"] + (["-d", build["distribution"]] if build.get("distribution") else [])
        return wsl + ["-e", "bash", "-lc", build_script(build, target_windows=target)]
    return ["bash", "-lc", build_script(build, target=target)]


def run_build(recipe, out):
    build = recipe["build"]
    started = time.time()
    result = {"ref": build.get("ref", ""), "config": build.get("config", "RelWithDebInfo")}
    log = out / "build.log"
    try:
        done = subprocess.run(build_argv(build, out), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=float(build.get("timeout") or 1800))
        output, code = done.stdout, done.returncode
    except subprocess.TimeoutExpired as error:
        output, code = error.stdout or b"", "timeout"
    except OSError as error:
        output, code = str(error).encode(), "cannot start"
    log.write_bytes(output)
    text = output.decode("utf-8", errors="replace")
    result["seconds"] = round(time.time() - started, 1)
    result["exit_code"] = code
    match = re.search(r"android_run: commit ([0-9a-f]{40})", text)
    result["commit"] = match.group(1) if match else ""
    result["tail"] = text.strip().splitlines()[-12:]
    result["status"] = "ok" if code == 0 and (out / "app.apk").is_file() else "failed"
    return result


def file_facts(path):
    data = Path(path).read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def run_install(recipe, adb, apk):
    package = device(recipe)["package"]
    result = {"apk": file_facts(apk), "before": package_times(adb, package)}
    code, out, err = adb.run("install", "-r", str(apk), timeout=600)
    result["output"] = (out + "\n" + err).strip().splitlines()[-5:]
    result["after"] = package_times(adb, package)
    before, after = result["before"].get("firstInstallTime"), result["after"].get("firstInstallTime")
    result["data_kept"] = bool(before) and before == after
    result["status"] = "ok" if code == 0 and "Success" in out else "failed"
    return result


# ----------------------------------------------------------------- a launch

class Logcat:
    """adb logcat from a device time on, read on a thread of its own."""

    def __init__(self, adb, since):
        self.process = adb.popen("logcat", "-v", "threadtime", "-T", since)
        self.lines = queue.Queue()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for raw in self.process.stdout:
            self.lines.put(raw.decode("utf-8", errors="replace").rstrip("\r\n"))

    def take(self):
        taken = []
        while True:
            try:
                taken.append(self.lines.get_nowait())
            except queue.Empty:
                return taken

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.thread.join(timeout=5)
        return self.take()


def wait_for_idle_app(recipe, adb, package):
    """None when the app is not running (or was stopped as the recipe asks),
    else the pids that kept it busy."""
    waited = 0
    while True:
        pids = app_pids(adb, package)
        if not pids:
            return None
        if recipe.get("stop_running_app"):
            adb.shell(f"am force-stop {package}")
            return None
        if waited >= float(recipe.get("wait_for_app", WAIT_FOR_RUNNING_APP)):
            return pids
        time.sleep(5)
        waited += 5


def wake_screen(adb):
    _, out, _ = adb.shell("dumpsys power", timeout=30)
    match = re.search(r"mWakefulness=(\w+)", out)
    if match and match.group(1) != "Awake":
        adb.shell("input keyevent KEYCODE_WAKEUP")
        return match.group(1)
    return None


def run_launch(recipe, adb, launch, out):
    target = device(recipe)
    package = target["package"]
    folder = out / launch["name"]
    result = {"name": launch["name"], "extra": extra_text(recipe, launch), "start": start_command(recipe, launch)}
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)

    busy = wait_for_idle_app(recipe, adb, package)
    if busy:
        result["status"] = "blocked"
        result["blocked_by"] = f"the app was already running (pid {', '.join(map(str, busy))}) and the recipe " \
                               "does not say stop_running_app"
        return result
    adb.shell(f"am force-stop {package}")
    woke = wake_screen(adb)
    if woke:
        result["woke_screen_from"] = woke
    shots_before = device_pictures(adb, package)
    _, now, _ = adb.shell("date '+%m-%d %H:%M:%S.000'", timeout=20)
    logcat = Logcat(adb, now.strip())
    time.sleep(0.5)  # logcat is reading before the app starts

    code, out_text, err = adb.shell(result["start"], timeout=60)
    started = time.time()
    result["start_output"] = (out_text + "\n" + err).strip().splitlines()[-4:]
    if code != 0 or "Error" in out_text or "Error" in err:
        result["status"] = "start_failed"
        logcat.stop()
        return result

    events = {"start": started}
    steps = sorted(({**step, "after": step.get("after", "start")} for step in launch.get("host") or []),
                   key=lambda step: (HOST_AFTER.index(step["after"]), step["at"]))
    done_steps, pids_seen, messages, crashes = [], set(), [], []
    all_lines, game_lines = [], []
    timeout = float(launch.get("timeout") or recipe.get("timeout") or 300)
    tags = set(recipe.get("logcat_tags") or DEFAULT_TAGS)
    last_pid_check, gone_at, status, processes = 0.0, None, None, {}

    def take(lines):
        for line in lines:
            all_lines.append(line)
            match = LOGCAT_LINE.match(line)
            if not match:
                continue
            pid, tag, message = int(match.group(2)), match.group(5), match.group(6)
            ours = pid in pids_seen or package in message
            if tag in tags or ours:
                game_lines.append(line)
            if tag == "KisakCOD":
                pids_seen.add(pid)
                messages.append(message)
                for event, text in EVENT_LINES.items():
                    if event not in events and (says_quit(message) if event == "quit" else message.strip() == text):
                        events[event] = time.time()
            if CRASH.search(message) and (ours or tag == "KisakCOD"):
                crashes.append(line)

    while True:
        take(logcat.take())
        now_time = time.time()
        # Every step whose moment has come, earliest first. A step waits only
        # for its own moment: a late "start" step must not hold back a
        # "map" step that is due sooner (item 332b: start +90 held map +15
        # and +35 until the launch ended).
        while True:
            due = [step for step in steps if step["after"] in events
                   and now_time >= events[step["after"]] + step["at"]]
            if not due:
                break
            step = min(due, key=lambda step: events[step["after"]] + step["at"])
            steps.remove(step)
            done_steps.append(run_host_step(adb, step, folder, events, now_time))
            now_time = time.time()
        if now_time - last_pid_check >= PID_EVERY:
            last_pid_check = now_time
            pids = game_pids(adb, package, processes)
            pids_seen.update(pids)
            if pids:
                gone_at = None
            elif pids_seen or now_time - started > NEVER_STARTED:
                gone_at = gone_at or now_time
        if crashes:
            # A crash once the game had said it was quitting still is one,
            # but everything the launch asked for has run (item 332b: SIGABRT
            # in an hwui thread after "window detached").
            status = "crash_at_quit" if "quit" in events else "crash"
        elif gone_at and now_time - gone_at >= 1:
            status = "ok" if "quit" in events else "died"
        elif "quit" in events and now_time - events["quit"] >= QUIT_GRACE:
            status = "ok"
            result["force_stopped_after_quit"] = True
        elif now_time - started >= timeout:
            status = "timeout"
        if status:
            break
        time.sleep(POLL)

    adb.shell(f"am force-stop {package}")
    take(logcat.stop())
    result["status"] = status
    result["seconds"] = round(time.time() - started, 1)
    result["events"] = {event: round(moment - started, 2) for event, moment in events.items()}
    result["pids"] = sorted(pids_seen)
    result["crash_lines"] = crashes[:20]
    (folder / "logcat-all.txt").write_text("\n".join(all_lines) + "\n", encoding="utf-8")
    (folder / "logcat.txt").write_text("\n".join(game_lines) + "\n", encoding="utf-8")

    text = "\n".join(messages)
    result["lines"] = pc_run.wanted_lines(text, recipe.get("lines") or [])
    result["frame_logs"] = [line for line in messages if pc_run.FRAME_LOG.match(line.strip())]
    result["run_errors"] = [line for line in messages if "none of its commands will be sent" in line]
    if result["run_errors"] and result["status"] == "ok":
        result["status"] = "run_error"
    if launch.get("variants"):
        result["variants"] = pc_run.variant_summary(launch, result["frame_logs"])

    for step in steps:
        done_steps.append({**step, "ok": False, "not_run": f"the {step['after']} never came" if
                           step["after"] not in events else "the launch ended first"})
    result["host_steps"] = done_steps

    shots = {}
    for step in done_steps:
        if step["do"] == "screencap" and step.get("ok"):
            shots[step["name"]] = str(folder / f"{step['name']}.png")
    for path, stamp in sorted(device_pictures(adb, package).items()):
        if shots_before.get(path) != stamp:
            name = path.rsplit("/", 1)[-1]
            code, _, err = adb.run("pull", path, str(folder / name), timeout=120)
            if code == 0 and (folder / name).is_file():
                shots[name[:-4]] = str(folder / name)
            else:
                result.setdefault("pull_errors", []).append(f"{path}: {err}")
    result["shots"] = shots
    wanted = pc_run.launch_shots(launch) + [step["name"] for step in launch.get("host") or []
                                            if step.get("do") == "screencap"]
    result["missing_shots"] = [name for name in wanted if name not in shots]
    return result


def run_host_step(adb, step, folder, events, now_time):
    done = {**step, "late_s": round(now_time - (events[step["after"]] + step["at"]), 2)}
    if step["do"] == "screencap":
        code, data, err = adb.run("exec-out", "screencap", "-p", timeout=60, binary=True)
        if code == 0 and data.startswith(PNG_SIGNATURE):
            (folder / f"{step['name']}.png").write_bytes(data)
            done["ok"] = True
        else:
            done["ok"], done["error"] = False, err or "no PNG came back"
    else:
        code, out, err = adb.shell(host_command(step), timeout=30)
        done["ok"] = code == 0
        if code:
            done["error"] = (out + " " + err).strip()
    return done


# ----------------------------------------------------------------- the report

def launch_ok(launch):
    return launch.get("status") == "ok" and not launch.get("missing_shots") and \
        all(step.get("ok") for step in launch.get("host_steps") or [])


def report_text(results):
    lines = [f"# {results['title']}", "", f"Recipe {results['id']}, run {results['finished']}. "
             "Local run on a test machine, not CI evidence.", ""]
    build = results.get("build")
    if build:
        lines.append(f"- build: {build['status']}, {build.get('ref') or 'the tree as it was'} at "
                     f"{build.get('commit') or '?'}, {build['config']}, {build.get('seconds')} s "
                     f"(exit {build.get('exit_code')})")
        if build["status"] != "ok":
            lines += ["```"] + build.get("tail", []) + ["```"]
    install = results.get("install")
    if install:
        apk = install["apk"]
        lines.append(f"- install -r: {install['status']}; APK {apk['bytes']} B, sha256 {apk['sha256']}; "
                     f"first install {install['before'].get('firstInstallTime', '-')} -> "
                     f"{install['after'].get('firstInstallTime', '-')} (data kept: {install['data_kept']}), "
                     f"last update {install['after'].get('lastUpdateTime', '-')}")
    emulator = results.get("emulator")
    if emulator:
        lines.append(f"- emulator {emulator['avd']} ({emulator['serial']}): {emulator.get('status')}"
                     + (f" in {emulator['boot_seconds']} s" if "boot_seconds" in emulator else "")
                     + ("; shut down after" if emulator.get("stopped") else ""))
    files = results.get("game_files")
    if files:
        lines.append(f"- game files ({files['check']}): {files['status']}"
                     + (f" in {files['seconds']} s" if "seconds" in files else ""))
    if results.get("stopped"):
        lines.append(f"- stopped: {results['stopped']}")
    lines += ["", "| Launch | Status | Seconds | Menu at | Map at | Quit at | Pictures | Missing | Host steps |",
              "| --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for launch in results["launches"]:
        events = launch.get("events") or {}
        steps = launch.get("host_steps") or []
        lines.append(f"| {launch['name']} | {launch.get('status', '-')} | {launch.get('seconds', '-')} | "
                     f"{events.get('menu', '-')} | {events.get('map', '-')} | {events.get('quit', '-')} | "
                     f"{len(launch.get('shots') or {})} | {', '.join(launch.get('missing_shots') or []) or '-'} | "
                     f"{sum(1 for step in steps if step.get('ok'))}/{len(steps)} |")
    for launch in results["launches"]:
        lines += ["", f"## {launch['name']}", ""]
        if launch.get("extra"):
            lines.append(f"- args: `{launch['extra']}`")
        for key in ("blocked_by", "woke_screen_from", "force_stopped_after_quit"):
            if launch.get(key):
                lines.append(f"- {key.replace('_', ' ')}: {launch[key]}")
        for step in launch.get("host_steps") or []:
            what = step["name"] if step["do"] == "screencap" else host_command(step)
            state = "ok" if step.get("ok") else step.get("not_run") or step.get("error") or "failed"
            lines.append(f"- {step['after']} + {step['at']} s: {what} ({state}"
                         f"{', ' + str(step['late_s']) + ' s late' if 'late_s' in step else ''})")
        for key in ("run_errors", "crash_lines", "pull_errors"):
            if launch.get(key):
                lines += [f"- {key.replace('_', ' ')}:", "```"] + launch[key] + ["```"]
        lines += pc_run.launch_report_lines(launch)
    lines += pc_run.picture_report_lines(results.get("pictures") or {})
    lines += ["", f"results.json sha256 {results['sha256']}", ""]
    return "\n".join(lines)


# ------------------------------------------------------------------- the run

def run_recipe(recipe, out, dry_run=False):
    target = device(recipe)
    adb = Adb(recipe)
    if dry_run:
        if recipe.get("emulator"):
            print(f"emulator: {subprocess.list2cmdline(emulator_argv(recipe))}")
        if recipe.get("build"):
            print("build:\n  " + subprocess.list2cmdline(build_argv(recipe["build"], out)[:-1]) + " <<script>>")
            print("  " + build_script(recipe["build"], target=(out / "app.apk")).replace("\n", "\n  "))
        apk = (out / "app.apk") if recipe.get("build") else recipe.get("apk")
        if apk and recipe.get("install", True):
            print(f"install: {subprocess.list2cmdline(adb.base + ['install', '-r', str(apk)])}")
        for launch in recipe["launches"]:
            print(f"{launch['name']}: {subprocess.list2cmdline(adb.base + ['shell', start_command(recipe, launch)])}")
            print(f"  the game's command line, {len(game_command_line(recipe, launch))} of {COMMAND_LINE_BYTES} "
                  "bytes")
            for step in launch.get("host") or []:
                what = "screencap " + step["name"] if step["do"] == "screencap" else host_command(step)
                print(f"  {step.get('after', 'start')} + {step['at']} s: {what}")
        return None
    out.mkdir(parents=True, exist_ok=True)
    results = {"format": FORMAT, "id": recipe.get("id", ""), "title": recipe.get("title", recipe.get("id", "")),
               "device": target, "launches": []}
    emulator = Emulator(recipe, adb, out) if recipe.get("emulator") else None
    try:
        run_on_device(recipe, adb, out, results, emulator)
    finally:
        if emulator:
            emulator.stop()
            results["emulator"] = emulator.result
    results["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    results["pictures"] = pc_run.pictures(recipe, results["launches"], out)
    pc_run.seal(results)
    (out / "results.json").write_text(json.dumps(results, indent=1, sort_keys=True))
    (out / "report.md").write_text(report_text(results), encoding="utf-8")
    print(f"android_run: report {out / 'report.md'}", flush=True)
    return results


def run_on_device(recipe, adb, out, results, emulator):
    target = device(recipe)
    apk = None
    if recipe.get("build"):
        # First, so an emulator does not sit booted through the build.
        print("android_run: building ...", flush=True)
        results["build"] = run_build(recipe, out)
        print(f"android_run: build {results['build']['status']} in {results['build']['seconds']} s", flush=True)
        if results["build"]["status"] != "ok":
            results["stopped"] = "the build failed (build.log)"
            return
        apk = out / "app.apk"
    elif recipe.get("apk"):
        apk = Path(recipe["apk"])
        if not apk.is_file():
            results["stopped"] = f"no APK at {apk}"
            return
    if emulator:
        print(f"android_run: emulator {recipe['emulator']['avd']} ...", flush=True)
        if not emulator.start():
            results["stopped"] = f"the emulator: {emulator.result['status']}"
            return
        print(f"android_run: emulator {emulator.result['status']}", flush=True)
    code, state, err = adb.run("get-state", timeout=30)
    if code != 0 or state.strip() != "device":
        results["stopped"] = f"no device: adb get-state said {(state or err).strip()!r}"
        return
    if not results.get("stopped") and apk and recipe.get("install", True):
        if app_pids(adb, target["package"]) and not recipe.get("stop_running_app"):
            results["stopped"] = "the app is running on the device (someone may be playing); not installing over it"
        else:
            print("android_run: adb install -r ...", flush=True)
            results["install"] = run_install(recipe, adb, apk)
            if results["install"]["status"] != "ok":
                results["stopped"] = "adb install -r failed"
    if not results.get("stopped") and recipe.get("game_files"):
        print("android_run: game files ...", flush=True)
        results["game_files"] = run_game_files(recipe, adb)
        if results["game_files"]["status"] not in ("already there", "copied"):
            results["stopped"] = f"the game files: {results['game_files']['status']}"
    if not results.get("stopped"):
        for launch in recipe["launches"]:
            print(f"android_run: {launch['name']} ...", flush=True)
            result = run_launch(recipe, adb, launch, out)
            print(f"android_run: {launch['name']}: {result.get('status')} in {result.get('seconds', 0)} s, "
                  f"{len(result.get('shots') or {})} pictures", flush=True)
            results["launches"].append(result)
            if result.get("status") == "blocked":
                results["stopped"] = "a launch found the app already running"
                break


def results_ok(recipe, results):
    if not results or results.get("stopped"):
        return False
    for key in ("build", "install"):
        if key in results and results[key]["status"] != "ok":
            return False
    if "game_files" in results and results["game_files"]["status"] not in ("already there", "copied"):
        return False
    return len(results["launches"]) == len(recipe["launches"]) and all(map(launch_ok, results["launches"]))


def run_queue(queue_folder, out_root, watch=0):
    out_root.mkdir(parents=True, exist_ok=True)
    lock = out_root / ".android_run.lock"
    try:
        handle = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        print(f"android_run: {lock} exists: another runner has this queue (delete the file if none does)",
              file=sys.stderr)
        return 2
    os.write(handle, str(os.getpid()).encode())
    os.close(handle)
    failed = 0
    try:
        while True:
            for path in sorted(queue_folder.glob("*.json")):
                if (queue_folder / "PAUSE").exists():
                    print("android_run: PAUSE found; stopping before the next recipe")
                    return 1 if failed else 0
                try:
                    recipe = load_recipe(path)
                except RecipeError as error:
                    out = out_root / path.stem
                    if not (out / "recipe-error.txt").exists():
                        out.mkdir(parents=True, exist_ok=True)
                        (out / "recipe-error.txt").write_text(f"{error}\n")
                        print(f"android_run: {path.name}: {error}", file=sys.stderr)
                        failed += 1
                    continue
                out = out_root / (recipe.get("id") or path.stem)
                if (out / "results.json").exists():
                    continue
                print(f"android_run: queue: {path.name} -> {out}", flush=True)
                shutil.copy2(path, out_root / f"{out.name}.recipe.json")
                results = run_recipe(recipe, out)
                failed += 0 if results_ok(recipe, results) else 1
            if not watch:
                return 1 if failed else 0
            time.sleep(watch)
    finally:
        lock.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("recipe", nargs="?")
    parser.add_argument("--out")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--queue")
    parser.add_argument("--out-root")
    parser.add_argument("--watch", type=float, default=0)
    options = parser.parse_args(argv)
    if options.queue:
        if not options.out_root or options.recipe:
            parser.error("--queue takes --out-root and no recipe")
        return run_queue(Path(options.queue), Path(options.out_root), options.watch)
    if not options.recipe or not options.out:
        parser.error("give a recipe and --out, or --queue and --out-root")
    try:
        recipe = load_recipe(options.recipe)
    except RecipeError as error:
        print(f"android_run: {error}", file=sys.stderr)
        return 2
    results = run_recipe(recipe, Path(options.out), dry_run=options.dry_run)
    if options.dry_run:
        return 0
    return 0 if results_ok(recipe, results) else 1


if __name__ == "__main__":
    sys.exit(main())
