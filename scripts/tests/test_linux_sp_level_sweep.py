#!/usr/bin/env python3
"""Self-test of scripts/tests/run_linux_sp_level_sweep.py, with no game.

The sweep drives KisakCOD-sp through its console. Here a small stand-in program
plays the game's part: it prints the console lines the sweep waits for, answers
``mapname`` (and, as the game does, an empty ``sv_mapname``), writes the save
file ``devsave`` would and the image ``screenshot`` would, loads again a moment
after ``kill``, and quits on ``quit``.
Each case runs the sweep against a stand-in tree (an empty ``main/`` and empty
``zone/english/<level>.ff`` placeholders) and checks the exit status, the
report and that nothing is written outside the work directory.

Also covered: the console-answer and gdb-status patterns, level name parsing,
and a stand-in that crashes, never loads, ignores ``quit``, draws one colour,
writes no image or stops at a death with no save.

Usage: python3 scripts/tests/test_linux_sp_level_sweep.py
"""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import stat
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
SWEEP_PATH = ROOT / "scripts" / "tests" / "run_linux_sp_level_sweep.py"

spec = importlib.util.spec_from_file_location("run_linux_sp_level_sweep", SWEEP_PATH)
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)

# The stand-in game. Behaviour per level comes from the environment:
# FAKE_CRASH_LEVEL dies with SIGSEGV after loading, FAKE_NOLOAD_LEVEL never
# prints the load line, FAKE_IGNORE_QUIT keeps running after quit,
# FAKE_FLAT_LEVEL draws one colour, FAKE_NO_SHOT_LEVEL writes no screenshot and
# FAKE_NO_SAVE_LEVEL has no save to load after a death.
FAKE_GAME = textwrap.dedent('''\
    #!{python}
    import os, signal, sys, time
    args = sys.argv[1:]
    def arg(name):
        return args[args.index(name) + 1] if name in args else None
    home = None
    for i, a in enumerate(args):
        if a == "+set" and args[i + 1] == "fs_homepath":
            home = args[i + 2]
    level = arg("+devmap")
    def load(name):
        global level
        level = name
        print("------ Server Initialization ------", flush=True)
        print("Server: " + name, flush=True)
        if name == os.environ.get("FAKE_NOLOAD_LEVEL"):
            return
        print("Load time: 1234 msec", flush=True)
        if name == os.environ.get("FAKE_ASSERT_LEVEL"):
            print("ASSERT FAILED: ent->health > 0\\n\\tg_main.cpp:42", file=sys.stderr, flush=True)
        if name == os.environ.get("FAKE_CRASH_LEVEL"):
            if os.environ.get("FAKE_CLOSE_BEFORE_CRASH"):
                # Output ends a moment before the exit status exists, as it
                # can on a busy machine.
                os.close(1)
                os.close(2)
                time.sleep(0.5)
            os.kill(os.getpid(), signal.SIGSEGV)
    load(level)
    for line in sys.stdin:
        cmd = line.split()
        if not cmd:
            continue
        if cmd[0] == "quit":
            if os.environ.get("FAKE_IGNORE_QUIT"):
                continue
            sys.exit(0)
        elif cmd[0] == "mapname":
            print('"mapname" is: "%s^7" default: "^7"' % level, flush=True)
        elif cmd[0] == "sv_mapname":
            # Singleplayer never sets this one (g_main.cpp registers it read-only).
            print('"sv_mapname" is: "^7" default: "^7"', flush=True)
        elif cmd[0] == "spdevmap":
            load(cmd[1])
        elif cmd[0] == "devsave":
            # As the game does when the level-start save is still pending:
            # that one is flushed first, with its own "time to write" line,
            # and this save's file appears a moment later.
            print("** Save was already written to a device; it is being saved again.  This is not an error.",
                  flush=True)
            print("time to write: 17  ms", flush=True)
            time.sleep(0.5)
            print("G_WriteGame '%s' ''" % cmd[1], flush=True)
            save = os.path.join(home, "players", "profiles", "sweep", "save")
            os.makedirs(save, exist_ok=True)
            open(os.path.join(save, cmd[1] + ".svg"), "wb").close()
            print("time to write: 3  ms", flush=True)
        elif cmd[0] == "loadgame":
            print("=== G_LoadGame ===", flush=True)
        elif cmd[0] == "screenshot":
            if level == os.environ.get("FAKE_NO_SHOT_LEVEL"):
                continue
            # A 32x32 top-first 24-bit TGA: bands of colour, or one grey.
            flat = level == os.environ.get("FAKE_FLAT_LEVEL")
            shots = os.path.join(home, "main", "screenshots")
            os.makedirs(shots, exist_ok=True)
            pixels = bytearray()
            for y in range(32):
                for x in range(32):
                    pixels += bytes((90, 90, 90) if flat else (x * 8, y * 8, 200))
            with open(os.path.join(shots, cmd[1] + ".tga"), "wb") as image:
                image.write(bytes([0, 0, 2]) + bytes(9) + bytes([32, 0, 32, 0, 24, 0x20]) + pixels)
        elif cmd[0] == "kill":
            time.sleep(0.5)
            if level == os.environ.get("FAKE_NO_SAVE_LEVEL"):
                print("ERROR: Save '' not available", flush=True)
            else:
                print("=== G_LoadGame ===", flush=True)
''')


class SweepPatternTests(unittest.TestCase):
    def test_console_answer_with_and_without_colour_codes(self):
        self.assertEqual(sweep.MAP_DVAR, "mapname")
        answer = sweep.dvar_answer(sweep.MAP_DVAR)
        self.assertEqual(answer.search('"mapname" is: "killhouse^7" default: "^7"').group(1), "killhouse")
        self.assertEqual(answer.search('"mapname" is: "killhouse" default: ""').group(1), "killhouse")
        self.assertIsNone(answer.search('"sv_mapname" is: "killhouse^7" default: "^7"'))
        self.assertIsNone(answer.search('"mapnames" is: "x"'))

    def test_gdb_status_lines(self):
        self.assertEqual(sweep.GDB_EXITED.search("[Inferior 1 (process 42) exited normally]").group(1), "normally")
        self.assertEqual(sweep.GDB_EXITED.search("[Inferior 1 (process 42) exited with code 01]").group(2), "01")
        self.assertEqual(sweep.GDB_SIGNAL.search("Thread 7 \"KisakCOD-sp\" received signal SIGSEGV, Segmentation fault.")
                         .group(1), "SIGSEGV")

    def test_level_names(self):
        self.assertEqual(sweep.parse_levels("all"), list(sweep.STOCK_LEVELS))
        self.assertEqual(len(sweep.STOCK_LEVELS), 21)
        self.assertEqual(sweep.parse_levels("killhouse, cargoship"), ["killhouse", "cargoship"])
        with self.assertRaises(ValueError):
            sweep.parse_levels("killhouse;rm")


class SweepRunTests(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="kisak-sp-sweep-test-")
        self.addCleanup(self._temporary.cleanup)
        root = Path(self._temporary.name)
        self.game = root / "game"
        (self.game / "main").mkdir(parents=True)
        (self.game / "zone" / "english").mkdir(parents=True)
        for level in ("killhouse", "cargoship", "coup"):
            (self.game / "zone" / "english" / f"{level}.ff").touch()
        self.binary = root / "fake-KisakCOD-sp"
        self.binary.write_text(FAKE_GAME.format(python=sys.executable))
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IXUSR)
        self.work = root / "work"
        self.saved_env = dict(os.environ)
        self.addCleanup(self.restore_env)

    def restore_env(self):
        os.environ.clear()
        os.environ.update(self.saved_env)

    def run_sweep(self, *extra):
        argv = [str(self.game), "--binary", str(self.binary), "--hold", "1", "--load-timeout", "10",
                "--probe-timeout", "5", "--quit-timeout", "5", "--hold-after-load", "1",
                "--work-dir", str(self.work), *extra]
        return sweep.main(argv), (self.work / "report.md").read_text()

    def test_levels_plan_passes_and_skips_missing_zones(self):
        status, report = self.run_sweep("--levels", "killhouse,cargoship,blackout")
        self.assertEqual(status, 0, report)
        self.assertIn("| killhouse | all steps seen | 1.2 s | exit 0 |", report)
        self.assertIn("| cargoship | all steps seen | 1.2 s | exit 0 |", report)
        self.assertIn("Levels without a zone file (not run): blackout", report)
        self.assertTrue((self.work / "killhouse.log").is_file())
        self.assertEqual((self.work / "home" / "players" / "profiles" / "active.txt").read_text(), "sweep\n")
        self.assertTrue((self.work / "base" / "main").is_symlink())
        self.assertEqual(sorted(path.name for path in self.game.rglob("*")),
                         ["cargoship.ff", "coup.ff", "english", "killhouse.ff", "main", "zone"],
                         "nothing was written into the game directory")

    def test_chain_plan_with_save_and_load(self):
        status, report = self.run_sweep("--plan", "chain", "--levels", "killhouse,cargoship,coup", "--save-load")
        self.assertEqual(status, 0, report)
        for level in ("killhouse", "cargoship", "coup"):
            self.assertIn(f"| {level} | all steps seen |", report)
        self.assertTrue((self.work / "home" / "players" / "profiles" / "sweep" / "save" / "sweep.svg").is_file())

    def test_crash_and_assertion_are_reported(self):
        os.environ["FAKE_CRASH_LEVEL"] = "cargoship"
        os.environ["FAKE_ASSERT_LEVEL"] = "cargoship"
        status, report = self.run_sweep("--levels", "killhouse,cargoship")
        self.assertEqual(status, 1)
        self.assertIn("| killhouse | all steps seen |", report)
        self.assertIn("| cargoship | stopped at: held 1 s | 1.2 s | signal 11 | 1 |", report)
        self.assertIn("- `ent->health > 0`", report)

    def test_crash_after_output_ends(self):
        os.environ["FAKE_CRASH_LEVEL"] = "cargoship"
        os.environ["FAKE_CLOSE_BEFORE_CRASH"] = "1"
        status, report = self.run_sweep("--levels", "cargoship")
        self.assertEqual(status, 1)
        self.assertIn("| cargoship | stopped at: held 1 s |", report)

    def test_level_that_never_loads(self):
        os.environ["FAKE_NOLOAD_LEVEL"] = "killhouse"
        os.environ["FAKE_IGNORE_QUIT"] = "1"
        status, report = self.run_sweep("--levels", "killhouse", "--load-timeout", "2")
        self.assertEqual(status, 1)
        self.assertIn("| killhouse | stopped at: loaded | - | killed after quit timed out |", report)

    def test_screenshot_is_measured_and_kept(self):
        status, report = self.run_sweep("--levels", "killhouse,cargoship", "--screenshot")
        self.assertEqual(status, 0, report)
        self.assertIn("screenshot: yes; death and retry: no;", report)
        self.assertIn("| Level | Size | Mean brightness | Not black | Colours |", report)
        self.assertRegex(report, r"\| killhouse \| 32x32 \| \d+ \| 100% \| 64 \| 2% \| #\w{6} \| #\w{6} \| #\w{6} \|")
        shots = self.work / "home" / "main" / "screenshots"
        self.assertEqual(sorted(path.name for path in shots.iterdir()), ["sweep_cargoship.tga", "sweep_killhouse.tga"])

    def test_screenshots_can_be_deleted_once_measured(self):
        status, report = self.run_sweep("--levels", "killhouse", "--screenshot", "--delete-screenshots")
        self.assertEqual(status, 0, report)
        self.assertIn("| killhouse | 32x32 |", report)
        self.assertEqual(list((self.work / "home" / "main" / "screenshots").iterdir()), [])

    def test_flat_and_missing_frames_are_reported_and_the_level_goes_on(self):
        os.environ["FAKE_FLAT_LEVEL"] = "killhouse"
        os.environ["FAKE_NO_SHOT_LEVEL"] = "cargoship"
        status, report = self.run_sweep("--plan", "chain", "--levels", "killhouse,cargoship,coup", "--screenshot",
                                        "--death-retry", "--probe-timeout", "2")
        self.assertEqual(status, 1)
        self.assertIn("| killhouse | stopped at: frame drawn |", report)
        self.assertIn("NOT SEEN: frame drawn (mean brightness 90 of 255, 1 colours, one colour on 100% of it)", report)
        self.assertIn("| cargoship | stopped at: frame drawn |", report)
        self.assertIn("NOT SEEN: frame drawn (no main/screenshots/sweep_cargoship.tga)", report)
        # A bad frame is reported, but the death and the next level still run.
        self.assertIn("- ok: retry loaded after death", report)
        self.assertIn("| coup | all steps seen |", report)

    def test_death_and_retry(self):
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            status, report = self.run_sweep("--levels", "killhouse,cargoship", "--save-load", "--death-retry")
        self.assertEqual(status, 0, report)
        self.assertIn("death and retry: yes;", report)
        self.assertIn("| killhouse | all steps seen |", report)
        for step in ("retry loaded after death", "held 1 s after the retry", "console answers after the retry"):
            self.assertIn(f"ok  cargoship: {step}", printed.getvalue())

    def test_death_with_no_save_is_reported(self):
        os.environ["FAKE_NO_SAVE_LEVEL"] = "cargoship"
        status, report = self.run_sweep("--levels", "killhouse,cargoship", "--death-retry", "--death-timeout", "2")
        self.assertEqual(status, 1)
        self.assertIn("| killhouse | all steps seen |", report)
        self.assertIn("| cargoship | stopped at: retry loaded after death | 1.2 s | exit 0 | 0 | 1 |", report)
        self.assertIn("NOT SEEN: retry loaded after death (no '=== G_LoadGame ===' in 2 s after kill)", report)
        self.assertIn("- `ERROR: Save '' not available`", report)

    def test_options_that_need_each_other(self):
        for extra in (("--delete-screenshots",), ("--death-retry", "--death-timeout", "0")):
            with self.assertRaises(SystemExit) as stopped:
                sweep.main([str(self.game), "--binary", str(self.binary), "--work-dir", str(self.work), *extra])
            self.assertEqual(stopped.exception.code, 2)

    def test_chain_stops_after_a_failed_level(self):
        os.environ["FAKE_NOLOAD_LEVEL"] = "cargoship"
        status, report = self.run_sweep("--plan", "chain", "--levels", "killhouse,cargoship,coup",
                                        "--load-timeout", "2")
        self.assertEqual(status, 1)
        self.assertIn("| killhouse | all steps seen |", report)
        self.assertIn("| cargoship | stopped at: loaded |", report)
        self.assertIn("| coup | stopped at: reached |", report)


if __name__ == "__main__":
    unittest.main()
