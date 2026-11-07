#!/usr/bin/env python3
"""Self-test of scripts/tests/run_linux_sp_level_sweep.py, with no game.

The sweep drives KisakCOD-sp through its console. Here a small stand-in program
plays the game's part: it prints the console lines the sweep waits for, answers
``mapname`` (and, as the game does, an empty ``sv_mapname``), writes the save
file ``devsave`` would, and quits on ``quit``.
Each case runs the sweep against a stand-in tree (an empty ``main/`` and empty
``zone/english/<level>.ff`` placeholders) and checks the exit status, the
report and that nothing is written outside the work directory.

Also covered: the console-answer and gdb-status patterns, level name parsing,
and a stand-in that crashes, never loads or ignores ``quit``.

Usage: python3 scripts/tests/test_linux_sp_level_sweep.py
"""

import importlib.util
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
# prints the load line, FAKE_IGNORE_QUIT keeps running after quit.
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

    def test_level_that_never_loads(self):
        os.environ["FAKE_NOLOAD_LEVEL"] = "killhouse"
        os.environ["FAKE_IGNORE_QUIT"] = "1"
        status, report = self.run_sweep("--levels", "killhouse", "--load-timeout", "2")
        self.assertEqual(status, 1)
        self.assertIn("| killhouse | stopped at: loaded | - | killed after quit timed out |", report)

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
