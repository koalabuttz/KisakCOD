#!/usr/bin/env python3
"""Check scripts/runs/android_run.py on the host with a stand-in adb.

The stand-in (tests/runs/fake_adb.py) is adb, the device, the emulator and
the app at once: the app builds its command line from the ``args`` extra the
way KisakActivity and src/linux/main.cpp do, logs to a fake logcat, shows the
menu and the map, runs the timed commands and writes its screenshots. The
test checks the recipe checks, a whole run (install -r, the unquoted extra,
host steps timed from the menu and the map, screen captures, pulled
screenshots, the report and its checksum), variants, a crash, an app that
outlives its quit, no device, a failed install, an app someone is using, the
emulator with a first copy of the game files, a build, the queue, and the
PNG reader the picture numbers use for screen captures.

No device, no emulator, no game. Usage:
    python3 scripts/tests/test_android_run.py
"""

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "runs" / "android_run.py"
FAKE = ROOT / "tests" / "runs" / "fake_adb.py"
sys.path.insert(0, str(RUNNER.parent))
import android_run  # noqa: E402
import pc_run  # noqa: E402

FAILURES = []


def check(condition, what):
    if not condition:
        FAILURES.append(what)
        print(f"FAIL: {what}")


def write_recipe(work, launches, name="recipe.json", **extra):
    value = {"format": "kisak-android-run/1", "id": "test", "title": "android_run test",
             "adb": [sys.executable, str(FAKE)], "timeout": 20, "launches": launches,
             "lines": ["run: run_atMap", "Loading fastfile"]}
    value.update(extra)
    path = work / name
    path.write_text(json.dumps(value))
    return path


def run(work, path, out, *more, env=None, timeout=120):
    environment = dict(os.environ, FAKE_ADB_STATE=str(work / "device"))
    environment.update(env or {})
    return subprocess.run([sys.executable, str(RUNNER), str(path), "--out", str(out), *more],
                          capture_output=True, text=True, timeout=timeout, env=environment)


def calls(work):
    path = work / "device" / "calls.txt"
    return path.read_text() if path.exists() else ""


def sealed(results):
    body = {key: value for key, value in results.items() if key != "sha256"}
    return hashlib.sha256(pc_run.canonical(body).encode("ascii")).hexdigest() == results["sha256"]


def test_recipe_checks(work):
    long_list = [f"{i} setviewpos {i}00 {i}00 {i}00 {i}0 0 0 0 0 0 0 0 0 0 0 0 0" for i in range(1, 24)]
    bad = [
        ({"launches": [{"name": "a", "args": ["+set", "name", '"x y"']}]}, "quote"),
        ({"launches": [{"name": "a", "at_map": long_list}]}, "1023"),
        ({"launches": [{"name": "a", "at_map": ["45 +left"]}]}, "'+'"),
        ({"launches": [{"name": "a", "host": [{"at": 1, "do": "shell", "command": "rm -rf /"}]}]}, "do is"),
        ({"launches": [{"name": "a", "host": [{"at": 1, "do": "key", "code": "KEYCODE_A; reboot"}]}]}, "code is"),
        ({"launches": [{"name": "a", "host": [{"at": 1, "do": "text", "text": "a b"}]}]}, "text is"),
        ({"launches": [{"name": "a", "host": [{"after": "end", "at": 1, "do": "screencap", "name": "x"}]}]},
         "after is"),
        ({"launches": [{"name": "a", "at_map": ["1 screenshot x"],
                        "host": [{"at": 1, "do": "screencap", "name": "x"}]}]}, "named twice"),
        ({"device": {"package": "com.example.other"}, "launches": [{"name": "a"}]}, "org.kisakcod"),
        ({"emulator": {"exe": "emulator", "avd": "x", "port": 5555}, "launches": [{"name": "a"}]}, "even"),
        ({"emulator": {"exe": "emulator", "avd": "x"}, "device": {"serial": "abc"}, "launches": [{"name": "a"}]},
         "not both"),
        ({"apk": "a.apk", "build": {"source": "s"}, "launches": [{"name": "a"}]}, "not both"),
        ({"game_files": {"from": "x"}, "launches": [{"name": "a"}]}, "check"),
        ({"launches": [{"name": "a"}], "pictures": {"compare": [{"a": "a", "b": "b"}]}}, "which no launch takes"),
    ]
    for index, (extra, message) in enumerate(bad):
        launches = extra.pop("launches")
        path = write_recipe(work, launches, name=f"bad-{index}.json", **extra)
        try:
            android_run.load_recipe(path)
            check(False, f"recipe {index} refused ({message})")
        except pc_run.RecipeError as error:
            check(message in str(error), f"recipe {index}: {error!r} names {message!r}")


def test_whole_run(work):
    apk = work / "app.apk"
    apk.write_bytes(b"PK fake apk")
    launches = [
        {"name": "menu", "at_menu": ["0.6 quit"],
         "host": [{"after": "menu", "at": 0.1, "do": "screencap", "name": "menu-cap"},
                  {"after": "start", "at": 0.05, "do": "key", "code": "KEYCODE_BUTTON_START", "source": "gamepad"}]},
        {"name": "crash", "args": ["+devmap", "mp_crash"],
         "at_map": ["0.2 setviewpos 336 60 268 65", "0.3 screenshot crash-a", "0.8 quit"],
         "host": [{"after": "map", "at": 0.2, "do": "screencap", "name": "crash-cap"},
                  {"after": "map", "at": 0.25, "do": "tap", "x": 960, "y": 540}]},
    ]
    path = write_recipe(work, launches, apk=str(apk), args=["+set", "com_maxfps", "0"],
                        pictures={"regions": {"square": [100, 60, 164, 124]},
                                  "compare": [{"a": "crash-a", "b": "crash-a", "label": "same"}]})
    out = work / "out"
    done = run(work, path, out)
    check(done.returncode == 0, f"whole run exits 0 ({done.returncode}): {done.stdout[-800:]} {done.stderr[-800:]}")
    results = json.loads((out / "results.json").read_text())
    check(sealed(results), "results.json checksum")
    check(results["install"]["status"] == "ok" and results["install"]["data_kept"], "install -r kept the data")
    check(results["install"]["apk"]["sha256"] == hashlib.sha256(b"PK fake apk").hexdigest(), "APK checksum")
    made = calls(work)
    check("install -r" in made and "uninstall" not in made, "only install -r")
    menu, crash = results["launches"]
    for launch in (menu, crash):
        check(launch["status"] == "ok", f"{launch['name']} ok: {launch.get('status')}")
        check(all(step["ok"] for step in launch["host_steps"]), f"{launch['name']} host steps ran")
        check(not launch["missing_shots"], f"{launch['name']} nothing missing: {launch['missing_shots']}")
    check("menu" in menu["events"] and "map" not in menu["events"], "menu launch: menu, no map")
    check(crash["events"].get("map", 0) > crash["events"].get("menu", 99), "map after menu")
    # The extra went unquoted, and the game joined the timed list back up.
    check("+set run_atMap 0.2 setviewpos 336 60 268 65 | 0.3 screenshot crash-a | 0.8 quit" in crash["extra"],
          f"extra: {crash['extra']}")
    check('run: run_atMap "0.2 setviewpos 336 60 268 65 | 0.3 screenshot crash-a | 0.8 quit"' in crash["lines"],
          f"the game read the whole list: {crash['lines']}")
    check("--es args '+set com_maxfps 0 +set run_atMap" in crash["start"], f"start: {crash['start']}")
    check(Path(crash["shots"]["crash-a"]).read_bytes()[:3] == b"\x00\x00\x02", "pulled TGA")
    check(Path(crash["shots"]["crash-cap"]).read_bytes()[:8] == android_run.PNG_SIGNATURE, "screen capture PNG")
    check("input gamepad keyevent KEYCODE_BUTTON_START" in made and "input tap 960 540" in made, "input sent")
    check(results["pictures"]["means"]["crash-cap"]["whole"] == [90.0, 90.0, 90.0], "PNG mean colour")
    check(results["pictures"]["means"]["crash-a"]["square"] == [100.0, 100.0, 100.0], "TGA mean colour")
    check((out / "crash" / "logcat.txt").read_text().count("KisakCOD:") >= 5, "logcat kept")
    report = (out / "report.md").read_text()
    check("install -r: ok" in report and "| crash | ok |" in report and results["sha256"] in report, "report")
    check(made.count("am force-stop org.kisakcod.game") >= 4, "force-stop before and after each launch")


def test_variants(work):
    launches = [{"name": "v", "args": ["+devmap", "mp_crash"], "at_map": ["0.1 setviewpos 1 2 3 4"],
                 "variants": [{"name": "A", "set": {"r_fakeCost": "0"}}, {"name": "B", "set": {"r_fakeCost": "10"}}],
                 "variant_timing": {"start": 0.3, "settle": 0.05, "framelog": 0.05, "warmup": 0}}]
    path = write_recipe(work, launches, install=False)
    done = run(work, path, work / "out-v")
    check(done.returncode == 0, f"variants run exits 0: {done.stdout[-600:]}")
    launch = json.loads((work / "out-v" / "results.json").read_text())["launches"][0]
    variants = {entry["name"]: entry for entry in launch.get("variants") or []}
    check(variants.get("A", {}).get("mean_fps") == 100.0 and variants.get("B", {}).get("mean_fps") == 50.0,
          f"variant frame rates: {variants}")
    check(set(launch["shots"]) == {"v-A", "v-B"}, f"variant screenshots: {launch['shots']}")


def test_failures(work):
    launches = [{"name": "c", "args": ["+devmap", "mp_crash"], "at_map": ["2 quit"]}]
    path = write_recipe(work, launches)
    done = run(work, path, work / "out-crash", env={"FAKE_APP_CRASH": "1"})
    launch = json.loads((work / "out-crash" / "results.json").read_text())["launches"][0]
    check(done.returncode == 1 and launch["status"] == "crash", f"crash: {launch.get('status')}")
    check(any("Fatal signal 11" in line for line in launch["crash_lines"]), "crash line kept")

    launches = [{"name": "l", "at_menu": ["0.2 quit"]}]
    path = write_recipe(work, launches)
    started = time.time()
    done = run(work, path, work / "out-linger", env={"FAKE_APP_LINGER": "1"})
    launch = json.loads((work / "out-linger" / "results.json").read_text())["launches"][0]
    check(done.returncode == 0 and launch["status"] == "ok" and launch.get("force_stopped_after_quit"),
          f"an app that outlives its quit is stopped: {launch}")
    check(time.time() - started < 30, "linger ends after the grace")

    for env, words in (({"FAKE_ADB_OFFLINE": "1"}, "no device"), ({"FAKE_INSTALL_FAIL": "1"}, "install -r failed")):
        apk = work / "app.apk"
        apk.write_bytes(b"PK")
        path = write_recipe(work, [{"name": "x", "at_menu": ["0.2 quit"]}], apk=str(apk))
        done = run(work, path, work / "out-fail", env=env)
        results = json.loads((work / "out-fail" / "results.json").read_text())
        check(done.returncode == 1 and words in results.get("stopped", "") and not results["launches"],
              f"{env}: stopped with {words!r}: {results.get('stopped')}")

    # Someone is using the app: no install over it, and a launch waits, then gives up.
    environment = dict(os.environ, FAKE_ADB_STATE=str(work / "device"))
    subprocess.run([sys.executable, str(FAKE), "shell", "am start -W -n org.kisakcod.game/.KisakActivity"],
                   env=environment, capture_output=True, timeout=30)
    path = write_recipe(work, [{"name": "x", "at_menu": ["0.2 quit"]}], wait_for_app=0)
    done = run(work, path, work / "out-busy")
    results = json.loads((work / "out-busy" / "results.json").read_text())
    check(done.returncode == 1 and results["launches"][0]["status"] == "blocked", "a running app blocks the launch")
    check("pidof" in calls(work), "checked for the running app")
    subprocess.run([sys.executable, str(FAKE), "shell", "am force-stop org.kisakcod.game"], env=environment,
                   capture_output=True, timeout=30)


def test_emulator(work):
    game = work / "cod4"
    (game / "main").mkdir(parents=True)
    (game / "main" / "iw_00.iwd").write_bytes(b"iwd")
    (game / "zone" / "english").mkdir(parents=True)
    (game / "zone" / "english" / "x.ff").write_bytes(b"ff")
    (game / "players" / "profiles" / "my player").mkdir(parents=True)
    (game / "players" / "profiles" / "my player" / "config_mp.cfg").write_bytes(b"cfg")
    apk = work / "app.apk"
    apk.write_bytes(b"PK emu")
    state = work / "emu-device"
    launches = [{"name": "e", "at_menu": ["0.3 quit"],
                 "host": [{"after": "menu", "at": 0.1, "do": "screencap", "name": "emu-cap"}]}]
    path = write_recipe(work, launches, apk=str(apk),
                        emulator={"exe": [sys.executable, str(FAKE), "--emulator"], "avd": "kisak-a13-x64",
                                  "port": 5556, "boot_timeout": 20, "env": {"ANDROID_AVD_HOME": "E:/Android/avd"}},
                        game_files={"from": str(game), "check": "main/iw_00.iwd"})
    env = {"FAKE_ADB_EMULATOR": "1", "FAKE_ADB_STATE": str(state)}
    for attempt, expected in ((1, "copied"), (2, "already there")):
        done = run(work, path, work / f"out-emu-{attempt}", env=env)
        check(done.returncode == 0, f"emulator run {attempt} exits 0: {done.stdout[-800:]} {done.stderr[-400:]}")
        results = json.loads((work / f"out-emu-{attempt}" / "results.json").read_text())
        check(results["emulator"]["status"] == "booted" and results["emulator"].get("stopped"),
              f"emulator {attempt} booted and shut down: {results['emulator']}")
        check(results["game_files"]["status"] == expected, f"game files {attempt}: {results['game_files']}")
        check(results["launches"] and results["launches"][0]["status"] == "ok", f"emulator launch {attempt}")
    args = (state / "emulator-args.txt").read_text()
    for word in ("-avd kisak-a13-x64", "-port 5556", "-gpu host", "-no-window", "-no-snapshot-save",
                 "ANDROID_AVD_HOME=E:/Android/avd"):
        check(word in args, f"emulator started with {word}: {args}")
    files = state / "sdcard" / "Android/data/org.kisakcod.game/files"
    check((files / "zone/english/x.ff").is_file() and (files / "players/profiles/my player/config_mp.cfg").is_file(),
          "game files pushed, folders three deep and with a space made first")
    check("-s emulator-5556" in (state / "calls.txt").read_text(), "adb talks to emulator-5556")
    check(not (state / "booted").exists(), "the emulator is down after the run")

    done = run(work, path, work / "out-emu-fail", env={**env, "FAKE_EMULATOR_FAIL": "1"})
    results = json.loads((work / "out-emu-fail" / "results.json").read_text())
    check(done.returncode == 1 and "emulator" in results.get("stopped", ""), f"emulator fail: {results}")


def test_build(work):
    source = work / "src"
    script = source / "scripts" / "platform" / "android"
    script.mkdir(parents=True)
    (script / "build_apk.py").write_text(
        "import sys, pathlib\n"
        "a = sys.argv\n"
        "work = pathlib.Path(a[a.index('--work') + 1])\n"
        "work.mkdir(parents=True, exist_ok=True)\n"
        "(work / ('KisakCOD-' + a[a.index('--config') + 1] + '.apk')).write_bytes(b'PK built ' + ' '.join(a[1:]).encode())\n")
    build = {"source": str(source), "work": str(work / "build-work"), "ndk": "n", "sdk": "s", "gradle": "g",
             "config": "Debug", "python": sys.executable, "args": ["--gradle-arg", "--offline"]}
    path = write_recipe(work, [{"name": "b", "at_menu": ["0.2 quit"]}], build=build)
    done = run(work, path, work / "out-build")
    results = json.loads((work / "out-build" / "results.json").read_text())
    check(done.returncode == 0 and results["build"]["status"] == "ok", f"build: {results.get('build')}")
    installed = (work / "device" / "installed.apk").read_bytes()
    check(installed.startswith(b"PK built") and b"--config Debug --gradle-arg --offline" in installed,
          "the built APK was installed")
    with_ref = android_run.build_script({**build, "ref": "origin/claude/x", "work": "~/kisak-android"},
                                        target_windows="C:\\kisak\\runs\\x\\app.apk")
    check("git checkout -q --detach origin/claude/x" in with_ref and "git diff --quiet" in with_ref,
          "a ref is checked out only in a clean tree")
    check('"$HOME"/kisak-android/KisakCOD-Debug.apk' in with_ref and "wslpath -u" in with_ref, "WSL paths")
    argv = android_run.build_argv({**build, "wsl": True, "distribution": "Ubuntu"}, work)
    check(argv[:6] == ["wsl.exe", "-d", "Ubuntu", "-e", "bash", "-lc"], f"WSL build argv: {argv[:6]}")
    build["source"] = str(work / "missing")
    path = write_recipe(work, [{"name": "b", "at_menu": ["0.2 quit"]}], build=build)
    done = run(work, path, work / "out-build-fail")
    results = json.loads((work / "out-build-fail" / "results.json").read_text())
    check(done.returncode == 1 and results["build"]["status"] == "failed" and not results["launches"],
          "a failed build stops the run")


def test_queue(work):
    folder, out_root = work / "queue", work / "queue-out"
    folder.mkdir()
    write_recipe(folder, [{"name": "q", "at_menu": ["0.2 quit"]}], name="a.json", id="queued-a")
    write_recipe(folder, [{"name": "q q"}], name="b.json", id="queued-b")
    environment = dict(os.environ, FAKE_ADB_STATE=str(work / "device"))
    command = [sys.executable, str(RUNNER), "--queue", str(folder), "--out-root", str(out_root)]
    done = subprocess.run(command, capture_output=True, text=True, timeout=120, env=environment)
    check(done.returncode == 1, f"a bad recipe in the queue fails the pass: {done.returncode}")
    check((out_root / "queued-a" / "results.json").is_file(), "the good recipe ran")
    check((out_root / "b" / "recipe-error.txt").is_file(), "the bad recipe's error is kept")
    check((out_root / "queued-a.recipe.json").is_file(), "the recipe is kept beside its results")
    before = calls(work).count("am start")
    subprocess.run(command, capture_output=True, text=True, timeout=120, env=environment)
    check(calls(work).count("am start") == before, "a done recipe is not run again")
    (out_root / ".android_run.lock").write_text("1")
    done = subprocess.run(command, capture_output=True, text=True, timeout=60, env=environment)
    check(done.returncode == 2 and "another runner" in done.stderr, "the lock keeps a second runner off")


def png_with_filters(width, height, rgba):
    """Rows filtered in turn with None, Sub, Up, Average and Paeth."""
    step = 4
    stride = width * step
    rows = [bytes(rgba[y * stride:(y + 1) * stride]) for y in range(height)]
    raw, previous = bytearray(), bytes(stride)
    for y, row in enumerate(rows):
        kind = y % 5
        out = bytearray()
        for i in range(stride):
            a = row[i - step] if i >= step else 0
            b = previous[i]
            c = previous[i - step] if i >= step else 0
            if kind == 0:
                guess = 0
            elif kind == 1:
                guess = a
            elif kind == 2:
                guess = b
            elif kind == 3:
                guess = (a + b) >> 1
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                guess = a if pa <= pb and pa <= pc else b if pb <= pc else c
            out.append((row[i] - guess) & 0xFF)
        raw += bytes((kind,)) + out
        previous = row

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    data = zlib.compress(bytes(raw))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", data[:20]) + chunk(b"IDAT", data[20:]) + chunk(b"IEND", b""))


def test_png_reader(work):
    width, height = 13, 11
    rgba = bytes((x * 19 + y * 7) & 0xFF if c < 3 else 255 for y in range(height) for x in range(width)
                 for c in range(4))
    path = work / "filters.png"
    path.write_bytes(png_with_filters(width, height, rgba))
    w, h, rgb = pc_run.read_png(path)
    expected = bytes(value for i, value in enumerate(rgba) if i % 4 != 3)
    check((w, h) == (width, height) and rgb == expected, "PNG reader undoes every filter")
    check(pc_run.read_picture(path) == (w, h, rgb), "read_picture takes a PNG")


def main():
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        for test in (test_recipe_checks, test_png_reader, test_whole_run, test_variants, test_failures,
                     test_emulator, test_build, test_queue):
            folder = work / test.__name__
            folder.mkdir()
            test(folder)
    if FAILURES:
        print(f"FAIL: android_run, {len(FAILURES)} checks")
        return 1
    print("PASS: android_run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
