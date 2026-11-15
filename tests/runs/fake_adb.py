#!/usr/bin/env python3
"""A stand-in for adb and the Android app in scripts/tests/test_android_run.py.

As adb it answers the calls scripts/runs/android_run.py makes, with a device
kept in the folder FAKE_ADB_STATE: ``get-state``, ``install -r``, ``pull``,
``exec-out screencap -p`` (a small grey PNG), ``logcat`` (the lines the app
writes from now on, until it is stopped) and ``shell`` with ``date``,
``pidof``, ``am force-stop``, ``am start``, ``input``, ``dumpsys package``,
``dumpsys power`` and ``find ... -exec stat``. Every call goes to
``calls.txt``; the device's /sdcard is ``<state>/sdcard``.

``am start`` starts the app (this script with ``--app``) on its own: it
builds its command line the way KisakActivity and src/linux/main.cpp do
(the extra split at spaces; a piece with a space or a quote quoted again;
over 1023 bytes, it does not start), reads ``+set`` lines the way the
console does (``set`` joins every word after the name), shows the menu
after 0.2 s and, with ``+devmap``, the map 0.3 s later, and sends the timed
commands at their times: ``screenshot <name>`` writes a TGA (as
fake_pc_game.py), ``framelog`` prints a frame log line, ``<dvar> <value>``
sets r_fakeCost, ``quit`` quits. Its console goes to logcat under KisakCOD.

As the emulator (``--emulator -avd <name> -port <port> ...``) it writes its
arguments to ``emulator-args.txt``, boots in 0.3 s, mounts shared storage
2.5 s later (``ls``/``mkdir`` under /sdcard are refused before) and runs
until ``adb emu kill``; with FAKE_ADB_EMULATOR=1 adb finds a device only while it runs.
``push <folder>/. <device folder>/`` copies a folder in, ``shell ls`` says
whether a path is there.

The app's game runs in its own process (``pidof org.kisakcod.game:game``)
beside the launcher's, as AndroidManifest.xml has it; FAKE_ONE_PROCESS=1
gives the one process of older builds. FAKE_APP_EXIT=1: the game ends
without a word when the map is up.

FAKE_ADB_OFFLINE=1: no device. FAKE_EMULATOR_FAIL=1: the emulator exits at once. FAKE_INSTALL_FAIL=1: the install fails.
FAKE_APP_CRASH=1: the app crashes when the map is up; FAKE_APP_CRASH_AT_QUIT=1:
it crashes right after saying it quits. FAKE_APP_LINGER=1: the
app process stays after the game quits (SDL's activity). FAKE_ASLEEP=1: the
screen is asleep until a KEYCODE_WAKEUP.
"""

import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fake_pc_game  # noqa: E402

STATE = Path(os.environ.get("FAKE_ADB_STATE", "fake-adb-state"))
PACKAGE = "org.kisakcod.game"
APP_FOLDER = f"/storage/emulated/0/Android/data/{PACKAGE}/files"
PNG = None
LAUNCHER_PID = 4242


def device_path(path):
    for prefix in ("/storage/emulated/0/", "/sdcard/"):
        if path.startswith(prefix):
            return STATE / "sdcard" / path[len(prefix):]
    return STATE / "other" / path.lstrip("/")


def log(message, pid=None, tag="KisakCOD", level="I"):
    stamp = time.strftime("%m-%d %H:%M:%S", time.localtime()) + f".{int(time.time() * 1000) % 1000:03d}"
    pid = pid or os.getpid()
    with open(STATE / "logcat.txt", "a", encoding="utf-8") as handle:
        handle.write(f"{stamp} {pid:5d} {pid:5d} {level} {tag}: {message}\n")


def png(width=64, height=36, shade=90):
    import struct
    import zlib
    raw = b"".join(b"\x00" + bytes((shade, shade, shade, 255)) * width for _ in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def alive(name):
    path = STATE / name
    if not path.exists():
        return None
    pid = int(path.read_text())
    try:
        os.kill(pid, 0)
    except OSError:
        return None
    return pid


def storage_ready():
    """Shared storage: on an emulator it mounts a while after boot_completed."""
    return os.environ.get("FAKE_ADB_EMULATOR") != "1" or (STATE / "storage").exists()


def shell(command):
    words = shlex.split(command)
    if (words[:1] in (["ls"], ["mkdir"]) and any(word.startswith(("/sdcard", "/storage/emulated")) for word in words)
            and not storage_ready()):
        print(f"{words[0]}: '/sdcard/Android': Permission denied", file=sys.stderr)
        return 1
    if words[:2] == ["getprop", "sys.boot_completed"]:
        print("1" if os.environ.get("FAKE_ADB_EMULATOR") != "1" or (STATE / "booted").exists() else "")
    elif words[:2] == ["mkdir", "-p"]:
        for path in words[2:]:
            device_path(path).mkdir(parents=True, exist_ok=True)
    elif words[:1] == ["ls"]:
        words = [word for word in words if word != "-d"]
        if not device_path(words[1]).exists():
            print(f"ls: {words[1]}: No such file or directory", file=sys.stderr)
            return 1
        print(words[1])
    elif words[:1] == ["date"]:
        print(time.strftime("%m-%d %H:%M:%S.000"))
    elif words[:1] == ["pidof"]:
        # The game runs in "<package>:game", the launcher in "<package>",
        # unless FAKE_ONE_PROCESS=1 (builds from before the :game process).
        one = os.environ.get("FAKE_ONE_PROCESS") == "1"
        found = []
        for name in words[1:]:
            if name == PACKAGE + ":game" and not one and alive("app.pid"):
                found.append(alive("app.pid"))
            elif name == PACKAGE and one and alive("app.pid"):
                found.append(alive("app.pid"))
            elif name == PACKAGE and not one and (STATE / "launcher").exists():
                found.append(LAUNCHER_PID)
        if not found:
            return 1
        print(" ".join(map(str, found)))
    elif words[:2] == ["am", "force-stop"]:
        (STATE / "launcher").unlink(missing_ok=True)
        pid = alive("app.pid")
        if pid:
            os.kill(pid, 9)
            (STATE / "app.pid").unlink()
            log(f"Force stopping {PACKAGE}", pid=1000, tag="ActivityManager")
    elif words[:2] == ["am", "start"]:
        extra = words[words.index("--es") + 2] if "--es" in words else ""
        app = subprocess.Popen([sys.executable, __file__, "--app", extra], start_new_session=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (STATE / "app.pid").write_text(str(app.pid))
        (STATE / "launcher").write_text("1")
        print(f"Starting: Intent {{ cmp={words[words.index('-n') + 1]} }}\nStatus: ok")
    elif words[:1] == ["input"]:
        if words[-1] == "KEYCODE_WAKEUP":
            (STATE / "asleep").unlink(missing_ok=True)
    elif words[:3] == ["dumpsys", "package", PACKAGE]:
        installed = STATE / "installed.txt"
        first = "2026-11-15 08:33:16"
        last = installed.read_text().strip() if installed.exists() else first
        print(f"Packages:\n  Package [{PACKAGE}]\n    versionCode=1 minSdk=26\n    versionName=0.1\n"
              f"    firstInstallTime={first}\n    lastUpdateTime={last}")
    elif words[:2] == ["dumpsys", "power"]:
        print(f"  mWakefulness={'Asleep' if (STATE / 'asleep').exists() else 'Awake'}")
    elif words[:1] == ["find"]:
        root = device_path(words[1])
        if root.is_dir():
            for path in sorted(root.rglob("*.tga")):
                stat = path.stat()
                shown = words[1].rstrip("/") + "/" + path.relative_to(root).as_posix()
                print(f"{int(stat.st_mtime)} {stat.st_size} {shown}")
    else:
        print(f"/system/bin/sh: {words[0]}: inaccessible or not found", file=sys.stderr)
        return 127
    return 0


def logcat():
    path = STATE / "logcat.txt"
    path.touch()
    with open(path, encoding="utf-8") as handle:
        handle.seek(0, 2)
        while True:
            line = handle.readline()
            if line:
                sys.stdout.write(line)
                sys.stdout.flush()
            else:
                time.sleep(0.02)


def adb(argv):
    with open(STATE / "calls.txt", "a", encoding="utf-8") as handle:
        handle.write(" ".join(argv) + "\n")
    if argv[:1] == ["-s"]:
        argv = argv[2:]
    emulator_gone = os.environ.get("FAKE_ADB_EMULATOR") == "1" and not alive("emulator.pid")
    if os.environ.get("FAKE_ADB_OFFLINE") == "1" or emulator_gone:
        print("error: no devices/emulators found", file=sys.stderr)
        return 1
    kind = argv[0]
    if kind == "get-state":
        print("device")
    elif kind == "install":
        if os.environ.get("FAKE_INSTALL_FAIL") == "1":
            print("Performing Streamed Install\nadb: failed to install: Failure [INSTALL_FAILED_TEST]")
            return 1
        (STATE / "installed.txt").write_text(time.strftime("%Y-%m-%d %H:%M:%S"))
        shutil.copy2(argv[-1], STATE / "installed.apk")
        print("Performing Streamed Install\nSuccess")
    elif argv[:2] == ["emu", "kill"]:
        pid = alive("emulator.pid")
        if pid:
            os.kill(pid, 15)
        print("OK: killing emulator, bye bye")
    elif kind == "push":
        source, target = Path(argv[1].rstrip(".").rstrip("/\\")), device_path(argv[2].rstrip("/"))
        # As the Android 13 x86_64 emulator image: adbd makes one new folder
        # level for a file, no more ("remote secure_mkdirs failed").
        for path in source.rglob("*"):
            parent = (target / path.relative_to(source)).parent
            if not parent.exists() and not parent.parent.exists():
                print(f"adb: error: failed to copy '{path}': remote secure_mkdirs failed: Operation not permitted",
                      file=sys.stderr)
                return 1
        shutil.copytree(source, target, dirs_exist_ok=True)
        print(f"{argv[1]}: {sum(1 for _ in source.rglob('*'))} files pushed")
    elif kind == "pull":
        shutil.copy2(device_path(argv[1]), argv[2])
        print(f"{argv[1]}: 1 file pulled")
    elif argv[:3] == ["exec-out", "screencap", "-p"]:
        sys.stdout.buffer.write(png())
    elif kind == "logcat":
        logcat()
    elif kind == "shell":
        return shell(" ".join(argv[1:]))
    else:
        print(f"adb: unknown command {kind}", file=sys.stderr)
        return 1
    return 0


# ------------------------------------------------------------------ the app

def tokenize(line):
    """The console's tokenizer, for what matters here: words, and quoted
    strings with \\" inside."""
    words, i = [], 0
    while i < len(line):
        if line[i].isspace():
            i += 1
        elif line[i] == '"':
            j, word = i + 1, ""
            while j < len(line) and line[j] != '"':
                if line[j] == "\\" and j + 1 < len(line) and line[j + 1] == '"':
                    j += 1
                word += line[j]
                j += 1
            words.append(word)
            i = j + 1
        else:
            j = i
            while j < len(line) and not line[j].isspace():
                j += 1
            words.append(line[i:j])
            i = j
    return words


def app(extra):
    pieces = ["+set", "fs_basepath", APP_FOLDER, "+set", "fs_homepath", APP_FOLDER + "/home",
              "+set", "r_fullscreen", "1"] + extra.split()
    log("arguments: [" + ", ".join(pieces) + "]")
    quoted = ['"' + p.replace('"', '\\"') + '"' if any(c in p for c in ' \t"') else p for p in pieces]
    command_line = " ".join(quoted)
    if len(command_line) > 1023:
        log("command line too long", level="E")
        return 1
    dvars, devmap = {}, None
    for part in command_line.split("+")[1:32]:
        words = tokenize(part)
        if len(words) >= 3 and words[0] == "set":
            dvars[words[1]] = " ".join(words[2:])
        elif words[:1] == ["devmap"]:
            devmap = words[1]
    home = device_path(dvars.get("fs_homepath", APP_FOLDER + "/home"))
    home.mkdir(parents=True, exist_ok=True)
    log(f"run: run_atMap \"{dvars.get('run_atMap', '')}\"")
    started = time.time()
    plan = []
    for key, event, offset in (("run_atMenu", "menu", 0.2), ("run_atMap", "map", 0.5)):
        if event == "map" and not devmap:
            continue
        for part in dvars.get(key, "").split("|"):
            words = part.split(None, 1)
            if len(words) == 2:
                plan.append((offset + float(words[0]), words[1].strip()))
    if devmap and dvars.get("run_quitAfterMap"):
        plan.append((0.5 + float(dvars["run_quitAfterMap"]), "-quitAfterMap"))
    plan.append((0.2, "-menu"))
    if devmap:
        plan.append((0.5, "-map"))
    plan.sort(key=lambda item: item[0])
    for at, text in plan:
        time.sleep(max(0.0, started + at - time.time()))
        if text == "-menu":
            log("run: the menu is up")
            continue
        if text == "-map":
            log(f"Loading fastfile {devmap}_load")
            log("run: the map is up")
            if os.environ.get("FAKE_APP_EXIT") == "1":
                return 0
            if os.environ.get("FAKE_APP_CRASH") == "1":
                log("Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 1 (SDLThread)",
                    tag="libc", level="F")
                return 1
            continue
        if text == "-quitAfterMap":
            # The game's own quit timer: the only time it says "quitting as asked".
            log(f"run: run_quitAfterMap {dvars['run_quitAfterMap']} reached")
            log("run: quitting as asked")
            text = "quit"
        else:
            # A timed "quit" is only sent, as by src/client/cl_run_plan.h.
            log(f"run: sending \"{text}\"")
        for command in text.split(";"):
            words = command.split()
            if not words:
                continue
            if words[0] == "screenshot":
                fake_pc_game.tga(home / "main" / "screenshots" / f"{words[1]}.tga", 100,
                                 int(dvars.get("r_fakeSquare", 100)), False)
                log(f"Wrote screenshots/{words[1]}.tga")
            elif words[0] == "framelog":
                ms = 10.0 + float(dvars.get("r_fakeCost", 0))
                log(f"frame log {words[2] if len(words) > 2 else '-'}: 1000 frames in {ms:.3f} s: "
                    f"{1000 / ms:.2f} fps; ms mean {ms:.3f}, median {ms:.3f}, p95 {ms + 1:.3f}, "
                    f"p99 {ms + 2:.3f}, max {ms + 3:.3f}; 0 over 50 ms")
            elif words[0] == "quit":
                log("quitting...")
                if os.environ.get("FAKE_APP_CRASH_AT_QUIT") == "1":
                    log("Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 2 (hwuiTask0)", tag="libc", level="F")
                    return 1
                if os.environ.get("FAKE_APP_LINGER") == "1":
                    time.sleep(3600)
                return 0
            elif len(words) == 2 and words[0].startswith("r_"):
                dvars[words[0]] = words[1]
    time.sleep(3600)  # the menu stays up
    return 0


def emulator(argv):
    (STATE / "emulator-args.txt").write_text(" ".join(argv) + "\n"
                                             + f"ANDROID_AVD_HOME={os.environ.get('ANDROID_AVD_HOME', '')}\n")
    if os.environ.get("FAKE_EMULATOR_FAIL") == "1":
        return 1
    (STATE / "emulator.pid").write_text(str(os.getpid()))
    (STATE / "booted").unlink(missing_ok=True)
    (STATE / "storage").unlink(missing_ok=True)
    time.sleep(0.3)
    (STATE / "booted").write_text("1")
    # Shared storage mounts later than boot_completed, as on the PC's AVD.
    time.sleep(2.5)
    (STATE / "storage").write_text("1")
    try:
        time.sleep(3600)
    finally:
        (STATE / "booted").unlink(missing_ok=True)
        (STATE / "storage").unlink(missing_ok=True)
    return 0


def main():
    STATE.mkdir(parents=True, exist_ok=True)
    (STATE / "sdcard" / "Android" / "data").mkdir(parents=True, exist_ok=True)  # as on any device
    if sys.argv[1:2] == ["--emulator"]:
        import signal
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        return emulator(sys.argv[2:])
    if sys.argv[1:2] == ["--app"]:
        try:
            return app(sys.argv[2])
        finally:
            if (STATE / "app.pid").exists() and (STATE / "app.pid").read_text() == str(os.getpid()):
                (STATE / "app.pid").unlink()
    return adb(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
