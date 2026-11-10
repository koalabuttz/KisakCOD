#!/usr/bin/env python3
"""Check the test machines' run driver and report writer with a stand-in game.

``scripts/runs/perf_run.py`` launches the client row by row on a test machine,
plays a scene through its console and writes a checksummed results file;
``scripts/runs/run_report.py`` turns that file into the report (docs/runs.md).
Neither can meet the real game on hosted CI, so this test builds
``tests/runs/fake_game.cpp``, a program that answers the same console lines,
and runs whole recipes against it: the order of the rows, the scene steps, the
figures read from the frame log line, the power, load, leftover and guard
checks, a stop by a PAUSE file and the resume after it, and the checksum that
tells a results file that crossed whole from one that was retyped.

No game data, no GPU and no Steam install: the power supplies are a folder of
text files and the game is the stand-in.

Usage:
    python3 scripts/tests/test_perf_run.py [--cxx c++] [--timeout 300]
"""

import argparse
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "runs"))
import perf_run  # noqa: E402
import run_report  # noqa: E402

FAILURES = []


def check(condition, what):
    if not condition:
        FAILURES.append(what)
        print(f"  FAIL: {what}")
    return condition


def set_power(root, ac, battery):
    """A made-up /sys/class/power_supply: one charger, one battery."""
    for name, kind in (("ADP1", "Mains"), ("BAT0", "Battery")):
        (root / name).mkdir(parents=True, exist_ok=True)
        (root / name / "type").write_text(kind + "\n")
    (root / "ADP1" / "online").write_text("1\n" if ac else "0\n")
    (root / "BAT0" / "capacity").write_text(f"{battery}\n")


class Bench:
    """One temporary folder with the stand-in game, a power tree and a guard folder."""

    def __init__(self, work, game):
        self.work = Path(work)
        self.game = game
        self.power = self.work / "power"
        self.guard = self.work / "guard"
        (self.guard / "deep" / "deeper").mkdir(parents=True)
        (self.guard / "file.txt").write_text("kept")
        (self.guard / "deep" / "deeper" / "file.txt").write_text("kept")
        self.home = self.work / "home-template"
        (self.home / "players").mkdir(parents=True)
        (self.home / "players" / "config.cfg").write_text("// made up\n")
        self.count = 0
        set_power(self.power, True, 80)
        perf_run.POWER_SUPPLY = self.power
        perf_run.read_profile = lambda: "power-saver"

    def recipe(self, **changes):
        recipe = {
            "format": "kisak-run/1",
            "id": "self-test",
            "title": "A self-test run",
            "asker": "the test",
            "game": {"binary": str(self.game), "args": ["+set", "row", "{row}"], "env": {"FAKE_BOTS_STEP": "6"},
                     "args_after": ["+devmap", "mp_fake"], "clear_env": ["KISAK_FAKE_*"],
                     "home_template": str(self.home), "quit_timeout": 10},
            "variants": {"A": {"env": {"FAKE_FPS": "100", "FAKE_SETTING": "a"}},
                         "B": {"env": {"FAKE_FPS": "50", "FAKE_SETTING": "b"}, "args": ["+set", "extra", "1"],
                               "note": "the slow one"}},
            "rows": ["A", "B"],
            "scene": [
                {"wait_quiet": 0.3, "timeout": 20},
                {"send": "sv_serverid"},
                {"expect": "\"sv_serverid\" is: \"(?P<serverid>\\d+)", "timeout": 10},
                {"send": ["mr {serverid} 8 autoassign", "viewpos"], "gap": 0.05},
                {"expect": "^\\(-716 461 ", "timeout": 10},
                {"send": "set scr_testclients 12"},
                {"poll": "status", "count": "bot\\d+\\^7", "at_least": 12, "every": 0.25, "timeout": 10,
                 "name": "bots"},
                {"framelog": 0.4, "slack": 10},
            ],
            "lines": ["^variant setting:", "^leftover setting:", "^\\] mr "],
            "counts": {"made_up": "made-up"},
            "power": {"require": "same", "battery_floor": 10, "battery_per_launch": 8, "profile": "power-saver",
                      "wait": 0.3, "retries": 1, "watch_every": 0.05},
            "load": {"max_cpu_percent": 100000, "window": 0.1},
            "guard": [str(self.guard), str(self.guard / "deep") + "/**"],
        }
        for key, value in changes.items():
            if isinstance(value, dict) and isinstance(recipe.get(key), dict):
                recipe[key] = {**recipe[key], **value}
            else:
                recipe[key] = value
        return recipe

    def run(self, recipe, *extra, out=None):
        """Runs a recipe through the command line. Returns (status, results, out folder, printed)."""
        self.count += 1
        out = Path(out) if out else self.work / f"out-{self.count}"
        path = self.work / f"recipe-{self.count}.json"
        path.write_text(json.dumps(recipe))
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed), contextlib.redirect_stderr(printed):
            status = perf_run.main([str(path), "--out", str(out), *extra])
        results = None
        if (out / "results.json").exists():
            results = json.loads((out / "results.json").read_text())
        return status, results, out, printed.getvalue()


def test_small_parts(bench):
    print("small parts")
    line = ("frame log A-1: 16928 frames in 120.004 s: 141.06 fps; ms mean 7.089, median 6.877, p95 11.126, "
            "p99 12.878, max 159.429; 1 over 50 ms")
    match = perf_run.FRAME_LOG.search("prefix " + line)
    check(match and match.group("label") == "A-1" and match.group("fps") == "141.06"
          and match.group("over50") == "1" and match.group("why") is None, "the frame log line is read")
    match = perf_run.FRAME_LOG.search(line.replace(" A-1", "") + " (at the quit)")
    check(match and match.group("label") is None and match.group("why") == "at the quit",
          "a frame log line with no label and an early end is read")

    check(perf_run.row_names(["A", "D", "D", "A"]) == [("A-1", "A"), ("D-1", "D"), ("D-2", "D"), ("A-2", "A")],
          "rows are named by variant and launch")

    home = os.path.expanduser("~")
    check(perf_run.expand("~/x/{row}", {"row": "A-1"}) == f"{home}/x/A-1", "~ and a name expand")
    check(perf_run.expand("a=~/x:~/y", {}) == f"a={home}/x:{home}/y", "~ expands inside a path list")
    check(perf_run.expand("say {unknown} {row}", {"row": "r"}) == "say {unknown} r", "an unknown name stays")

    check(perf_run.read_power(bench.power) == {"ac": True, "battery": 80}, "the charger and battery are read")
    usb = bench.work / "usb-power"
    (usb / "ucsi").mkdir(parents=True)
    (usb / "ucsi" / "type").write_text("USB\n")
    (usb / "ucsi" / "online").write_text("0\n")
    check(perf_run.read_power(usb) == {"ac": False, "battery": None}, "a USB-C charger counts as the mains")
    check(perf_run.read_power(bench.work / "nothing") == {"ac": None, "battery": None},
          "a machine with no power supplies says nothing")

    proc = bench.work / "proc"
    for pid, name, ticks in ((10, "busy (one) two)", 100), (11, "idle", 5)):
        (proc / str(pid)).mkdir(parents=True)
        fields = ["S"] + ["0"] * 10 + [str(ticks), "0"] + ["0"] * 20
        (proc / str(pid) / "stat").write_text(f"{pid} ({name}) " + " ".join(fields) + "\n")
    (proc / "self").mkdir()
    times = perf_run._process_times(proc)
    check(times == {10: ("busy (one) two)", 100), 11: ("idle", 5)}, "process names with brackets are read")
    real_sleep = perf_run.time.sleep
    ticks_per_second = os.sysconf("SC_CLK_TCK")

    def work_then_return(seconds):
        fields = ["S"] + ["0"] * 10 + [str(100 + int(ticks_per_second * seconds * 1.5)), "0"] + ["0"] * 20
        (proc / "10" / "stat").write_text("10 (busy (one) two)) " + " ".join(fields) + "\n")
    perf_run.time.sleep = work_then_return
    try:
        load = perf_run.sample_load(2.0, root=proc)
    finally:
        perf_run.time.sleep = real_sleep
    check(load == [("busy (one) two)", 150.0)], f"the share of a core a process used is worked out ({load})")
    check(perf_run.running_names(["idle", "absent"], proc) == ["idle"], "a named process is found running")

    before = perf_run.snapshot([str(bench.guard), str(bench.guard / "deep") + "/**"])
    check(any(name.endswith("deeper/file.txt") for name in before), "a /** guard folder is listed all the way down")
    check(perf_run.compare_snapshots(before, dict(before)) == "ok", "an unchanged guard is ok")
    after = dict(before)
    after[str(bench.guard / "new.bin")] = (3, 0)
    check(perf_run.compare_snapshots(before, after) == "changed: 1 added (new.bin)", "a new file is named")


def test_recipes_refused(bench):
    print("recipes that are refused")
    good = bench.recipe()
    cases = {
        "another format": {"format": "kisak-run/0"},
        "a row that is no variant": {"rows": ["A", "Z"]},
        "no rows": {"rows": []},
        "an id with a slash": {"id": "a/b"},
        "a step of two kinds": {"scene": [{"send": "x", "sleep": 1}]},
        "a step of no kind": {"scene": [{"timeout": 1}]},
        "a pattern that does not compile": {"lines": ["("]},
        "an expect that does not compile": {"scene": [{"expect": "("}]},
        "a poll with no count": {"scene": [{"poll": "status"}]},
        "an after step that is no tool": {"after": [{"send": "x"}]},
        "an unknown power rule": {"power": {"require": "sometimes"}},
    }
    for what, change in cases.items():
        status, results, _, printed = bench.run({**good, **change})
        check(status == 2 and results is None and "perf_run.py:" in printed, f"{what} is refused ({status})")
    path = bench.work / "not-json.json"
    path.write_text("{")
    with contextlib.redirect_stderr(io.StringIO()):
        check(perf_run.main([str(path), "--out", str(bench.work / "never")]) == 2, "a broken file is refused")
    check(not (bench.work / "never").exists(), "a refused recipe makes no output folder")

    status, results, out, printed = bench.run(bench.recipe(rows=["A", "B", "B", "A"]), "--check")
    check(status == 0 and "4 launches: A-1 B-1 B-2 A-2" in printed and not out.exists(),
          "--check prints the plan and starts nothing")
    status, _, _, printed = bench.run(bench.recipe(game={"binary": str(bench.work / "no-such-game")}), "--check")
    check(status == 1 and "(NOT FOUND)" in printed, "--check says when the game is not there")


def test_two_builds(bench):
    print("two builds in one run")
    other = bench.work / "other-build" / bench.game.name
    other.parent.mkdir()
    shutil.copy2(bench.game, other)
    with open(other, "ab") as file:
        file.write(b"another build")
    recipe = bench.recipe(rows=["A", "B"])
    recipe["variants"]["B"]["binary"] = "{other}/" + bench.game.name
    status, _, _, printed = bench.run(recipe, "--check", "--set", f"other={other.parent}")
    check(status == 0 and printed.count("game: ") == 2 and "NOT FOUND" not in printed,
          f"--check names both builds ({printed.strip()})")
    status, results, out, _ = bench.run(recipe, "--set", f"other={other.parent}")
    rows = results["rows"]
    check(status == 0 and rows[0]["binary"]["sha256"] != rows[1]["binary"]["sha256"]
          and f"$ {other}" in (out / "B-1.log").read_text() and f"$ {bench.game}" in (out / "A-1.log").read_text(),
          "a variant runs its own binary and the rows tell the builds apart")
    check(f"binary sha256 {rows[1]['binary']['sha256']}" in run_report.render(results)
          and run_report.compact(results)["rows"][1]["binary"] == rows[1]["binary"],
          "the report and the short form say which build a row ran")


def test_machine_names(bench):
    print("a machine's names")
    recipe = bench.recipe(rows=["A"], game={"binary": "{build}/" + bench.game.name, "home_template": "{template}"},
                          guard=["{guarded}", "{guarded}/deep/**"])
    recipe["variants"]["A"]["env"]["FAKE_SETTING"] = "{flavour} of {row}"
    status, results, _, printed = bench.run(recipe)
    check(status == 2 and results is None and "uses {build}, {flavour}, {guarded}, {template}" in printed,
          f"a recipe that uses names nobody gave is refused ({printed.strip()})")
    names = bench.work / "names.json"
    names.write_text(json.dumps({"build": str(bench.game.parent), "template": str(bench.home),
                                 "guarded": str(bench.guard), "flavour": "file"}))
    status, results, _, printed = bench.run(recipe, "--names", str(names), "--set", "flavour=set")
    check(status == 0 and results["rows"][0]["status"] == "ok" and results["rows"][0]["guard"] == "ok"
          and results["rows"][0]["lines"]["^variant setting:"] == ["variant setting: set of A-1"],
          f"the names file and --set fill the recipe in ({printed.strip()[-200:]})")
    os.environ["KISAK_RUN_NAMES"] = str(names)
    try:
        status, results, _, _ = bench.run(recipe)
    finally:
        del os.environ["KISAK_RUN_NAMES"]
    check(status == 0 and results["rows"][0]["lines"]["^variant setting:"] == ["variant setting: file of A-1"],
          "KISAK_RUN_NAMES names the file too")
    status, _, _, printed = bench.run(recipe, "--names", str(names), "--set", "flavour")
    check(status == 2 and "--set takes name=value" in printed, "a --set with no value is refused")
    status, _, _, printed = bench.run(recipe, "--names", str(bench.work / "absent.json"))
    check(status == 2 and "absent.json" in printed, "a names file that is not there is refused")

    given = {"game", "build", "libs", "home_template", "mesa_cache", "dxvk_prefix"}
    kept = sorted((ROOT / "scripts" / "runs" / "recipes").glob("*.json"))
    check(bool(kept), "the repository keeps recipes")
    for path in kept:
        try:
            recipe = perf_run.load_recipe(path)
        except perf_run.RecipeError as error:
            check(False, f"{path.name} is a recipe the driver takes ({error})")
            continue
        missing = perf_run.missing_names(recipe, dict.fromkeys(given, "x"))
        text = path.read_text()
        check(not missing, f"{path.name} uses only the documented names ({missing})")
        check("/home/" not in text and "~" not in text and ":\\" not in text,
              f"{path.name} names no machine's folders")


def test_whole_run(bench):
    print("a whole run")
    os.environ["KISAK_FAKE_LEFT"] = "left over from the shell"
    tool = [sys.executable, "-c", "import sys; print('beside', sys.argv[1])", "{row}"]
    recipe = bench.recipe(rows=["A", "B", "B", "A"])
    recipe["scene"][-1]["with"] = {"run": tool, "name": "capture"}
    recipe["after"] = [{"run": [sys.executable, "-c", "import sys; print('after', sys.argv[1])", "{variant}"],
                        "name": "export"}]
    status, results, out, printed = bench.run(recipe)
    os.environ.pop("KISAK_FAKE_LEFT")
    if not check(status == 0 and results is not None, f"the run succeeds ({status})\n{printed}"):
        return
    rows = results["rows"]
    check(results["status"] == "complete" and run_report.verify(results) is None,
          "the results are complete and their checksum holds")
    check([row["name"] for row in rows] == ["A-1", "B-1", "B-2", "A-2"], "the rows ran in the recipe's order")
    check(all(row["status"] == "ok" and row["exit"] == 0 for row in rows), "every row is ok")
    check([row["framelog"][0]["fps"] for row in rows] == [100.0, 50.0, 50.0, 100.0], "each row has its frame rate")
    first = rows[0]
    check(first["framelog"][0]["label"] == "A-1" and first["framelog"][0]["frames"] == 40
          and first["framelog"][0]["line"].startswith("frame log A-1: 40 frames"), "the frame log line is kept")
    check(first["values"] == {"serverid": "42", "bots": 12}, f"what the scene read is kept ({first['values']})")
    check(first["lines"]["^variant setting:"] == ["variant setting: a"]
          and rows[1]["lines"]["^variant setting:"] == ["variant setting: b"], "each variant got its environment")
    check(first["lines"]["^leftover setting:"] == ["leftover setting: cleared"],
          "the shell's own setting was cleared")
    check(first["lines"]["^\\] mr "] == ["] mr 42 8 autoassign"], "a name read from the log went into a line")
    check(first["counts"]["warnings"] == 1 and first["counts"]["made_up"] == 1 and first["counts"]["errors"] == 0,
          "log lines are counted")
    check(first["outputs"] == {"capture": ["beside A-1"], "export": ["after A"]},
          f"the tools' output is kept ({first.get('outputs')})")
    check(first["guard"] == "ok" and first["power"] == {"ac": True, "battery_start": 80, "battery_end": 80,
                                                        "profile": "power-saver", "changed": False},
          "the guard and the power are noted")
    check(rows[1].get("note") == "the slow one", "a variant's note travels with its rows")
    check(len(first["binary"]["sha256"]) == 16 and first["binary"]["modified"].endswith("Z")
          and rows[1]["binary"] == first["binary"], "each row notes which binary it ran")
    log = (out / "A-1.log").read_text()
    check("argument 2: fs_homepath" in log and f"argument 3: {out / 'A-1' / 'home'}" in log
          and "argument 6: A-1" in log and "> framelog 0.4 A-1" in log, "the log has the command line and the sends")
    check((out / "A-1" / "home" / "players" / "config.cfg").exists(), "each row got a copy of the home template")
    check("argument 9: 1\nargument 10: +devmap\nargument 11: mp_fake" in (out / "B-1.log").read_text(),
          "a variant's arguments follow the game's, and the last ones follow both")

    report = (out / "report.md").read_text()
    check("# A self-test run" in report and "checksum verified" in report and "| A | 2 | 100.0 |" in report
          and "| B | 2 | 50.0 |" in report and "frame log B-2: 20 frames" in report and "bots 12" in report
          and "note: the slow one" in report and "mains, 80 to 80%, power-saver" in report,
          "the report has the tables, the lines and the facts")
    summary = run_report.summarize(results)
    check("4 of 4 rows ok" in summary and "A 100.0 (10.000, 20.000) from 2 [100.0, 100.0]" in summary,
          f"the summary has the averages ({summary})")
    check(len(run_report.summarize(results, limit=120)) <= 120, "the summary keeps to its limit")

    # The file as it crosses: inside a message, in a code fence.
    pasted = bench.work / "pasted.txt"
    text = (out / "results.json").read_text()
    pasted.write_text("Here are the results:\n\n```json\n" + text + "```\nThat is all.\n")
    with contextlib.redirect_stdout(io.StringIO()) as printed:
        status = run_report.main([str(pasted), "--check"])
    check(status == 0 and "checksum ok" in printed.getvalue(), "a pasted results file checks out")
    written = bench.work / "written.md"
    with contextlib.redirect_stdout(io.StringIO()):
        status = run_report.main([str(pasted), "--out", str(written)])
    check(status == 0 and written.read_text() == report, "the report from the paste is the machine's report")
    pasted.write_text(text.replace('"fps": 50.0', '"fps": 55.0', 1))
    with contextlib.redirect_stdout(io.StringIO()) as printed:
        status = run_report.main([str(pasted), "--out", str(written)])
    check(status == 1 and "does not match" in printed.getvalue()
          and "**Not the machine's own words" in written.read_text() and "checksum not verified" in written.read_text(),
          "a retyped figure fails the checksum and the report says so")
    with contextlib.redirect_stdout(io.StringIO()) as printed:
        status = run_report.main([str(pasted), "--summary"])
    check(status == 1 and "CHECKSUM PROBLEM" in printed.getvalue(), "so does the summary")
    pasted.write_text("no results here")
    with contextlib.redirect_stderr(io.StringIO()):
        check(run_report.main([str(pasted)]) == 2, "a file with no results is refused")

    # The short form: what a machine pastes when the whole file is too long.
    short_text = (out / "results-compact.json").read_text()
    short = json.loads(short_text)
    check(run_report.verify(short) is None and short["full_sha256"] == results["sha256"] and short["compact"],
          "the short form has its own checksum and names the file it was cut from")
    check(len(short_text) < len(text) / 3 and "\n" not in short_text.strip(),
          f"the short form is a third of the file or less, on one line ({len(short_text)} of {len(text)})")
    check(run_report.averages(short["rows"]) == run_report.averages(rows)
          and run_report.summarize(short) == summary, "the averages and the summary from it are the same")
    short_report = run_report.render(short)
    check("| A | 2 | 100.0 |" in short_report and "frame log B-2: 20 frames" in short_report
          and "From the short form" in short_report and "bots 12" in short_report,
          "a report from the short form has the tables and the frame log lines")
    pasted.write_text(text)
    with contextlib.redirect_stdout(io.StringIO()) as printed:
        status = run_report.main([str(pasted), "--compact"])
    check(status == 0 and printed.getvalue() == short_text, "--compact prints the same short form")
    # The wanted lines cross in the short form only when asked for, a few per pattern.
    check(all("lines" not in row for row in short["rows"]), "the short form has no wanted lines unless asked")
    made_up = json.loads(json.dumps(results))
    made_up["rows"][0]["lines"] = {"^memory": [f"memory line {index}" for index in range(9)],
                                   "^long": ["x" * 400], "^none": []}
    with_lines = run_report.compact(run_report.seal({k: v for k, v in made_up.items() if k != "sha256"}), lines=4)
    kept = with_lines["rows"][0].get("lines", {})
    check(kept.get("^memory") == ["memory line 0", "memory line 1", "[5 more]", "memory line 7", "memory line 8"]
          and kept.get("^long") == ["x" * 300 + " [cut]"] and "^none" not in kept
          and run_report.verify(with_lines) is None, f"the short form keeps a few wanted lines per pattern ({kept})")
    made_up["compact_lines"] = 1
    own = run_report.compact(run_report.seal({k: v for k, v in made_up.items() if k != "sha256"}))
    check(own["rows"][0]["lines"]["^memory"] == ["memory line 0", "[8 more]"],
          "the results' own compact_lines is the default")
    check("memory line 0" in run_report.render(with_lines) and "a few per pattern" in run_report.render(with_lines),
          "the report shows the kept lines and says they are a selection")
    pasted.write_text(short_text.replace("50.00 fps", "55.00 fps", 1))
    with contextlib.redirect_stdout(io.StringIO()) as printed:
        status = run_report.main([str(pasted), "--check"])
    check(status == 1 and "does not match" in printed.getvalue(), "a retyped frame log line fails its checksum")

    status, _, _, printed = bench.run(recipe, out=out)
    check(status == 2 and "--resume" in printed, "a finished output folder is not overwritten")


def test_rows_that_fail(bench):
    print("rows that fail")
    status, results, _, _ = bench.run(bench.recipe(rows=["A", "B"], variants={
        "A": {"env": {"FAKE_CRASH_ON": "framelog"}}, "B": {}}))
    rows = results["rows"]
    check(status == 1 and results["status"] == "complete, with rows not ok: A-1", "a crashed row fails the run")
    check(rows[0]["status"].startswith("failed: framelog") and "the game ended" in rows[0]["status"]
          and rows[0]["exit"] == 3 and rows[0]["counts"]["asserts"] == 1
          and any("ASSERT FAILED" in line for line in rows[0]["tail"]), f"the crash is described ({rows[0]['status']})")
    check(rows[1]["status"] == "ok", "the next row still runs")
    summary = run_report.summarize(results)
    check("1 of 2 rows ok" in summary and "A-1: failed: framelog" in summary and "A-1: asserts 1" in summary,
          "the summary names the failed row")
    report = run_report.render(results)
    check("- step failed: framelog" in report and "Last lines of the log:" in report, "the report shows the failure")

    status, results, _, _ = bench.run(bench.recipe(rows=["A"], variants={"A": {"env": {"FAKE_EXIT": "7"}}}))
    check(status == 1 and results["rows"][0]["status"] == "failed: exit 7", "an exit status that is not 0 fails")

    status, results, _, _ = bench.run(bench.recipe(rows=["A"], variants={"A": {"env": {"FAKE_IGNORE_QUIT": "1"}}},
                                                   game={"quit_timeout": 0.5}))
    check(status == 1 and results["rows"][0]["status"] == "failed: killed after the quit timed out"
          and results["rows"][0]["exit"] is None, "a game that will not quit is killed and the row fails")

    status, results, _, _ = bench.run(bench.recipe(rows=["A"], variants={"A": {"env": {"FAKE_EARLY": "1"}}}))
    check(status == 1 and "ended early (stopped)" in results["rows"][0]["status"]
          and results["rows"][0]["framelog"][0]["fps"] == 100.0, "a recording that ended early fails the row")

    recipe = bench.recipe(rows=["A"], variants={"A": {"env": {"FAKE_BOTS_STEP": "1"}}})
    recipe["scene"][6]["timeout"] = 0.6
    status, results, _, _ = bench.run(recipe)
    check(status == 1 and results["rows"][0]["status"].startswith("failed: poll (bots ")
          and not results["rows"][0]["framelog"], "bots that do not come fail the row before the capture")
    recipe["scene"][6]["optional"] = True
    status, results, _, _ = bench.run(recipe)
    check(status == 0 and results["rows"][0]["status"] == "ok" and results["rows"][0]["values"]["bots"] < 12
          and ["poll", False] == results["rows"][0]["steps"][6][:2], "an optional step's failure is noted only")

    recipe = bench.recipe(rows=["A"])
    recipe["scene"][2] = {"expect": "never printed", "timeout": 0.4}
    status, results, _, _ = bench.run(recipe)
    check(status == 1 and results["rows"][0]["status"] == "failed: expect (never printed)",
          "a line that never comes fails the row")

    status, results, _, _ = bench.run(bench.recipe(rows=["A"], game={"binary": str(bench.work / "no-such-game")}))
    check(status == 1 and results["rows"][0]["status"].startswith("failed: the game did not start"),
          "a missing binary fails the row")

    touched = bench.guard / "deep" / "deeper" / "written-by-the-game.bin"
    status, results, _, _ = bench.run(bench.recipe(rows=["A"], variants={"A": {"env": {"FAKE_TOUCH": str(touched)}}}))
    check(status == 1 and results["rows"][0]["status"] == "flagged: the guard saw a change"
          and "written-by-the-game.bin" in results["rows"][0]["guard"], "a file written into a guarded folder is flagged")
    summary = run_report.summarize(results)
    check("A-1: guard changed: " in summary, "and the summary says so")
    touched.unlink()

    other = subprocess.Popen([str(bench.game)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
    try:
        status, results, _, _ = bench.run(bench.recipe(rows=["A"]))
        check(status == 1 and "1 process(es) of this binary were still running" in results["rows"][0]["status"],
              "a row does not start while an earlier game still runs")
        status, results, _, _ = bench.run(bench.recipe(rows=["A"], stop_if_running=[bench.game.name]))
        check(status == 1 and results["status"] == f"stopped: before A-1, {bench.game.name} was running"
              and not results["rows"], "a run stops while a named program runs")
    finally:
        other.kill()
        other.wait()


def test_power(bench):
    print("power")
    set_power(bench.power, False, 17)
    status, results, _, _ = bench.run(bench.recipe())
    check(status == 1 and not results["rows"] and "the battery was at 17%" in results["status"]
          and results["status"].startswith("stopped: "), f"a battery too low for a launch stops the run "
          f"({results['status']})")
    set_power(bench.power, False, 18)
    status, results, _, _ = bench.run(bench.recipe(rows=["A"]))
    check(status == 0 and results["rows"][0]["power"]["ac"] is False, "a battery just high enough runs")
    check("battery, 18 to 18%" in run_report.render(results), "the report says the row ran on battery")

    status, results, _, _ = bench.run(bench.recipe(power={"require": "mains"}))
    check(status == 1 and results["status"] == "stopped: before A-1 the machine was not on mains after waiting 0 s"
          and not results["rows"], f"a round that needs the mains does not start on battery ({results['status']})")
    set_power(bench.power, True, 80)
    status, results, _, _ = bench.run(bench.recipe(power={"require": "battery"}))
    check(status == 1 and "was not on battery" in results["status"], "nor a battery round on the mains")

    perf_run.read_profile = lambda: "balanced"
    status, results, _, _ = bench.run(bench.recipe())
    check(status == 1 and results["status"] == "stopped: before A-1 the power profile was balanced, not power-saver",
          "another power profile stops the run")
    perf_run.read_profile = lambda: None
    status, results, _, _ = bench.run(bench.recipe(rows=["A"]))
    check(status == 0, "a machine with no power profiles runs")
    perf_run.read_profile = lambda: "power-saver"

    # The charger is pulled a moment into A-1's capture and put back after it.
    online = bench.power / "ADP1" / "online"
    marker = Path(str(online) + ".flipped")

    def put_back():
        deadline = time.monotonic() + 60
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        time.sleep(1.2)
        online.write_text("1\n")
    helper = threading.Thread(target=put_back, daemon=True)
    helper.start()
    recipe = bench.recipe(rows=["A", "B"], variants={"A": {"env": {"FAKE_FLIP": str(online), "FAKE_FPS": "100"}},
                                                     "B": {"env": {"FAKE_FPS": "50"}}},
                          power={"wait": 20})
    recipe["scene"][-1]["framelog"] = 0.8
    status, results, _, printed = bench.run(recipe)
    helper.join(30)
    names = [row["name"] for row in results["rows"]]
    check(status == 0 and results["status"] == "complete" and names == ["A-1.spoiled1", "A-1", "B-1"],
          f"a row whose power changed is run again ({status}, {names}, {results['status']})\n{printed}")
    if len(results["rows"]) == 3:
        spoiled, again = results["rows"][0], results["rows"][1]
        check(spoiled["status"] == "flagged: the power state changed during the capture"
              and spoiled["power"]["changed"] and again["status"] == "ok" and not again["power"]["changed"],
              "the spoiled launch is kept and marked, the second one counts")
        table = run_report.averages(results["rows"])
        check([(variant, count) for variant, count, _, _ in table] == [("A", 1), ("B", 1)],
              "the averages leave the spoiled launch out")
        check("changed during the capture**" in run_report.render(results), "the report marks the spoiled launch")
    marker.unlink(missing_ok=True)

    # Pulled and not put back: the run stops rather than measure in another state.
    recipe["power"]["wait"] = 0.5
    status, results, _, _ = bench.run(recipe)
    names = [row["name"] for row in results["rows"]]
    check(status == 1 and names == ["A-1.spoiled1"] and results["status"].startswith(
        "stopped: before A-1 the machine was not on mains"), f"a charger that stays out stops the run ({names})")
    marker.unlink(missing_ok=True)
    set_power(bench.power, True, 80)

    # A lid closed for 13 s while the first row ran (the laptop did this on
    # 2026-11-10): the row is kept, marked and run again.
    clock = iter([100.0, 113.1])
    real = perf_run.slept_seconds
    perf_run.slept_seconds = lambda: next(clock, 113.1)
    try:
        status, results, _, printed = bench.run(bench.recipe(rows=["A", "B"]))
    finally:
        perf_run.slept_seconds = real
    names = [row["name"] for row in results["rows"]]
    check(status == 0 and results["status"] == "complete" and names == ["A-1.spoiled1", "A-1", "B-1"],
          f"a row the machine slept through is run again ({status}, {names}, {results['status']})\n{printed}")
    if len(results["rows"]) == 3:
        slept, again = results["rows"][0], results["rows"][1]
        check(slept["status"] == "flagged: the power: the machine slept 13.1 s during the row"
              and slept["power"]["slept"] == 13.1 and again["status"] == "ok" and "slept" not in again["power"],
              f"the row is marked with the seconds slept ({slept['status']})")
        check("slept 13.1 s during the row**" in run_report.render(results), "the report marks the row slept through")
        compact = run_report.compact(results)
        check(compact["rows"][0]["power"].get("slept") == 13.1, "the short form keeps the mark")
    check(isinstance(real(), float) and real() >= 0.0, "the real suspend clock reads")


def test_load(bench):
    print("other load")
    samples = [[("cc1plus", 180.0), ("other", 20.0)], [("cc1plus", 150.0)], [("other", 20.0)], [("other", 5.0)]]
    real = perf_run.sample_load
    perf_run.sample_load = lambda window, exclude=(), root=None: samples.pop(0) if samples else []
    try:
        status, results, _, printed = bench.run(bench.recipe(rows=["A"], load={"max_cpu_percent": 50, "wait": 30,
                                                                              "window": 0.05, "every": 0.05}))
        row = results["rows"][0]
        check(status == 0 and printed.count("waiting: cc1plus") == 2 and row["load"]["top"] == [["other", 20.0]]
              and row["load"]["over"] is False and row["load"]["at_capture"] == [["other", 5.0]],
              f"a row waits for a busy process to end and notes the load ({row['load']})")
        samples[:] = [[("cc1plus", 180.0)]] * 50
        status, results, _, _ = bench.run(bench.recipe(rows=["A"], load={"max_cpu_percent": 50, "wait": 0.3,
                                                                         "window": 0.05, "every": 0.05}))
        row = results["rows"][0]
        check(status == 0 and row["load"]["over"] is True, "after its wait the row runs anyway, marked")
        check("still over the limit" in run_report.render(results), "and the report says so")
    finally:
        perf_run.sample_load = real


def test_pause_and_resume(bench):
    print("pause and resume")
    recipe = bench.recipe(rows=["A", "B", "A"])
    out = bench.work / "paused"
    out.mkdir()
    path = bench.work / "paused.json"
    path.write_text(json.dumps(recipe))

    def echo(text):
        if text == "A-1: ok":
            (out / "PAUSE").write_text("")
    run = perf_run.Run(perf_run.load_recipe(path), path.read_text(), out, echo=echo)
    check(run.run() is False and run.results["status"] == "stopped: a PAUSE file stopped the run before B-1",
          f"a PAUSE file stops the run between launches ({run.results['status']})")
    stopped = json.loads((out / "results.json").read_text())
    check(run_report.verify(stopped) is None and [row["name"] for row in stopped["rows"]] == ["A-1"],
          "the stopped run's results are whole")
    (out / "PAUSE").unlink()

    changed = bench.work / "changed.json"
    changed.write_text(json.dumps(bench.recipe(rows=["A", "B"])))
    with contextlib.redirect_stderr(io.StringIO()) as printed:
        status = perf_run.main([str(changed), "--out", str(out), "--resume"])
    check(status == 2 and "different recipe" in printed.getvalue(), "another recipe cannot resume it")

    with contextlib.redirect_stdout(io.StringIO()) as printed:
        status = perf_run.main([str(path), "--out", str(out), "--resume"])
    results = json.loads((out / "results.json").read_text())
    check(status == 0 and "A-1: already done" in printed.getvalue() and results["status"] == "complete"
          and [row["name"] for row in results["rows"]] == ["A-1", "B-1", "A-2"]
          and results["started"] == stopped["started"], "--resume runs the rows that were left")

    status, results, _, _ = bench.run(recipe, "--rows", "B,A-2")
    check(status == 0 and [row["name"] for row in results["rows"]] == ["B-1", "A-2"],
          "--rows runs the named rows and variants only")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kisak-perf-run-") as work:
        game = Path(work) / "kisak_fake_game"
        build = subprocess.run([args.cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
                                str(ROOT / "tests" / "runs" / "fake_game.cpp"), "-o", str(game)],
                               capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the stand-in game did not build")
            return 1
        bench = Bench(work, game)
        for test in (test_small_parts, test_recipes_refused, test_machine_names, test_two_builds, test_whole_run, test_rows_that_fail, test_power,
                     test_load, test_pause_and_resume):
            test(bench)
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} check(s) failed")
        return 1
    print("PASS: perf run driver and report")
    return 0


if __name__ == "__main__":
    sys.exit(main())
