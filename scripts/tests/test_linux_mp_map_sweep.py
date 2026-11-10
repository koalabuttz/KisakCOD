#!/usr/bin/env python3
"""Self-test of scripts/tests/run_linux_mp_map_sweep.py, with no game.

The sweep drives KisakCOD-mp through its console. Here a small stand-in program
plays the game's part: it prints ``Server: <map>``, answers ``clientinfo`` with
a loading state and then ``state: 9``, answers ``sv_serverId`` and ``mapname``,
lists configstrings with script menus, spawns its "player" only for the right
``mr`` responses under the current server id, logs a ``K;`` line when a
spawned player is killed, writes a TGA for ``screenshot`` and quits on
``quit``. Each case runs the sweep against a stand-in tree (an empty ``main/``
and empty ``zone/english/<map>.ff`` placeholders) and checks the exit status,
the report and that nothing is written outside the work directory.

Also covered: a first load that drops to the menu, menus that sit at other
positions on a later map, a join that never spawns, a flat-colour frame, a
script error, the sound counts, a crash, and a chain that stops.

Usage: python3 scripts/tests/test_linux_mp_map_sweep.py
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
SWEEP_PATH = ROOT / "scripts" / "tests" / "run_linux_mp_map_sweep.py"

spec = importlib.util.spec_from_file_location("run_linux_mp_map_sweep", SWEEP_PATH)
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)

# The stand-in game. Behaviour per map comes from the environment:
# FAKE_DROP_MAP drops the first load of that map back to the menu,
# FAKE_MOVED_MENUS_MAP registers its menus two positions later,
# FAKE_NO_SPAWN_MAP ignores the class response, FAKE_FLAT_MAP writes a
# one-colour screenshot, FAKE_SCRIPT_ERROR_MAP prints a script error,
# FAKE_CRASH_MAP dies with SIGSEGV after the join, FAKE_NOLOAD_MAP never
# leaves the loading state.
FAKE_GAME = textwrap.dedent('''\
    #!{python}
    import os, signal, struct, sys
    args = sys.argv[1:]
    def arg(name):
        return args[args.index(name) + 1] if name in args else None
    home = None
    for i, a in enumerate(args):
        if a == "+set" and args[i + 1] == "fs_homepath":
            home = args[i + 2]
    sound = "snd_alDebug" in args
    env = os.environ.get
    state = dict(map=None, server_id=0, polls=0, loads=dict(), team=False, alive=False, dropped=False)
    def load(name):
        state["map"] = name
        state["server_id"] += 16
        state["polls"] = 0
        state["team"] = state["alive"] = False
        state["dropped"] = False
        state["loads"][name] = state["loads"].get(name, 0) + 1
        print("------ Server Initialization ------", flush=True)
        print("Server: " + name, flush=True)
    def client_state():
        name = state["map"]
        state["polls"] += 1
        if state["dropped"] or name == env("FAKE_NOLOAD_MAP"):
            return 0 if state["dropped"] else 7
        if state["polls"] == 1:
            return 7
        if name == env("FAKE_DROP_MAP") and state["loads"][name] == 1:
            state["dropped"] = True
            print("********************", flush=True)
            print("ERROR: PLATFORM_STATSREADERROR", flush=True)
            print("********************", flush=True)
            return 0
        return 9
    def menus():
        shift = 2 if state["map"] == env("FAKE_MOVED_MENUS_MAP") else 0
        return {{8 + shift: "team_marinesopfor", 9 + shift: "class_marines", 15 + shift: "changeclass_mw"}}
    load(arg("+devmap"))
    for line in sys.stdin:
        cmd = line.split()
        if not cmd:
            continue
        name = state["map"]
        if cmd[0] == "quit":
            sys.exit(0)
        elif cmd[0] == "clientinfo":
            print("--------- Client Information ---------", flush=True)
            print("state: %d" % client_state(), flush=True)
            print("Server: localhost", flush=True)
        elif cmd[0] == "mapname":
            print('"mapname" is: "%s^7" default: "^7"' % name, flush=True)
        elif cmd[0] == "sv_serverId":
            print('"sv_serverid" is: "%d^7" default: "0^7"' % state["server_id"], flush=True)
        elif cmd[0] == "configstrings":
            print("   0: \\\\g_gametype\\\\war", flush=True)
            print(" 830: some/model", flush=True)
            for position, menu in sorted(menus().items()):
                print("%4d: %s" % (1970 + position, menu), flush=True)
        elif cmd[0] == "devmap":
            load(cmd[1])
        elif cmd[0] == "mr" and len(cmd) == 4 and int(cmd[1]) == state["server_id"]:
            menu = menus().get(int(cmd[2]))
            if menu == "team_marinesopfor" and cmd[3] == "autoassign":
                state["team"] = True
            elif menu == "changeclass_mw" and cmd[3] == "assault_mp,0" and state["team"]:
                state["alive"] = name != env("FAKE_NO_SPAWN_MAP")
                if name == env("FAKE_SCRIPT_ERROR_MAP"):
                    print("", flush=True)
                    print("******* script runtime error *******", flush=True)
                    print("undefined is not an object: (file 'maps/mp/x.gsc', line 1)", flush=True)
                if name == env("FAKE_ASSERT_MAP"):
                    print("ASSERT FAILED: ent->health > 0\\n\\tg_main.cpp:42", file=sys.stderr, flush=True)
                if name == env("FAKE_CRASH_MAP"):
                    os.kill(os.getpid(), signal.SIGSEGV)
        elif cmd[0] == "+attack" and sound:
            print("snd_al: last 5.0 s: 12 starts, 1 cut while playing, 0 stream underruns, 0 OpenAL errors; "
                  "playing 2D 1/8 3D 3/32 stream 1/13; longest update gap 31 ms", flush=True)
        elif cmd[0] == "kill" and state["alive"]:
            state["alive"] = False
            os.makedirs(os.path.join(home, "main"), exist_ok=True)
            with open(os.path.join(home, "main", "games_mp.log"), "a") as log:
                log.write("  0:42 K;0;0;allies;sweep;;-1;world;;none;100000;MOD_SUICIDE;none\\n")
        elif cmd[0] == "screenshot":
            folder = os.path.join(home, "main", "screenshots")
            os.makedirs(folder, exist_ok=True)
            width, height = 64, 32
            header = bytes([0, 0, 2]) + bytes(9) + struct.pack("<HHBB", width, height, 24, 0x20)
            flat = name == env("FAKE_FLAT_MAP")
            # Top first: a blue sky third, then a ramp of greys.
            def pixel(x, y):
                if flat:
                    return (0, 0, 0)
                return (224, 128, 64) if y < height // 3 else ((x * 4) % 256,) * 3
            pixels = bytes(v for y in range(height) for x in range(width) for v in pixel(x, y))
            with open(os.path.join(folder, cmd[1] + ".tga"), "wb") as shot:
                shot.write(header + pixels)
''')

SHORT_EXERCISE = (("+attack", 0.1), ("-attack", 0.1), ("weapnext", 0.1))


class SweepPatternTests(unittest.TestCase):
    def test_map_names(self):
        self.assertEqual(sweep.parse_maps("all"), list(sweep.STOCK_MAPS))
        self.assertEqual(len(sweep.STOCK_MAPS), 21)
        self.assertEqual(len(set(sweep.STOCK_MAPS)), 21)
        self.assertEqual(sweep.parse_maps("mp_crash, mp_bog"), ["mp_crash", "mp_bog"])
        with self.assertRaises(ValueError):
            sweep.parse_maps("mp_crash;quit")

    def test_script_menus_come_from_the_configstring_listing(self):
        menus = sweep.script_menus(["   0: \\g_gametype\\war", "1969: not a menu", "1978: team_marinesopfor",
                                    "1985: changeclass_mw ", "2002: past the menus", "state: 9"])
        self.assertEqual(menus, {8: "team_marinesopfor", 15: "changeclass_mw"})
        self.assertEqual(sweep.find_menu(menus, 8), (8, "team_marinesopfor"))
        self.assertEqual(sweep.find_menu(menus, "CHANGECLASS_MW"), (15, "changeclass_mw"))
        self.assertIsNone(sweep.find_menu(menus, 9))
        self.assertIsNone(sweep.find_menu(menus, "class"))
        self.assertEqual(sweep.menu_ref("15"), 15)
        self.assertEqual(sweep.menu_ref("changeclass"), "changeclass")

    def test_console_patterns(self):
        self.assertEqual(sweep.CLIENT_STATE.search("state: 9").group(1), "9")
        self.assertTrue(sweep.DEATH_LINE.match("  3:07 K;0;0;allies;name;;-1;world;;none;100000;MOD_SUICIDE;none"))
        self.assertFalse(sweep.DEATH_LINE.match("  3:07 J;0;0;name"))
        self.assertTrue(sweep.SCRIPT_ERROR.search("^1Error: ******* script runtime error *******"))
        summary = sweep.SOUND_SUMMARY.search(
            "snd_al: last 5.0 s: 103 starts, 8 cut while playing, 0 stream underruns, 2 OpenAL errors; "
            "playing 2D 1/8 3D 3/32 stream 1/13; longest update gap 7600 ms")
        self.assertEqual(summary.groups(), ("103", "8", "0", "2", "7600"))
        answer = sweep.re.compile(sweep.sp.dvar_answer(sweep.SERVER_ID_DVAR).pattern, sweep.re.I)
        self.assertEqual(answer.search('"sv_serverid" is: "16^7" default: "0^7"').group(1), "16")

    def test_frame_numbers(self):
        import struct
        with tempfile.TemporaryDirectory() as folder:
            def tga(name, width, height, descriptor, depth, pixel):
                header = bytes([0, 0, 2]) + bytes(9) + struct.pack("<HHBB", width, height, depth, descriptor)
                rows = range(height) if descriptor & 0x20 else reversed(range(height))
                path = Path(folder) / name
                path.write_bytes(header + bytes(v for y in rows for x in range(width) for v in pixel(x, y)))
                return path
            # Blue-green-red order in the file; a red top half over a black bottom half.
            halves = lambda x, y: (0, 0, 255, 255) if y < 8 else (0, 0, 0, 255)
            for descriptor in (0x20, 0):  # rows stored top first, then bottom first
                frame = sweep.Frame(tga("halves.tga", 16, 16, descriptor, 32, halves))
                self.assertEqual(frame.size, "16x16")
                self.assertEqual(frame.colours, 2)
                self.assertAlmostEqual(frame.lit, 0.5)
                self.assertAlmostEqual(frame.commonest, 0.5)
                self.assertEqual(frame.thirds[0], "#ff0000")
                self.assertEqual(frame.thirds[2], "#000000")
                self.assertFalse(frame.flat())
            self.assertTrue(sweep.Frame(tga("grey.tga", 16, 16, 0x20, 24, lambda x, y: (90, 90, 90))).flat())
            with self.assertRaises(ValueError):
                sweep.Frame(tga("short.tga", 16, 16, 0x20, 24, lambda x, y: (1, 2, 3) if y < 4 and x < 4 else ()))
            rle = Path(folder) / "rle.tga"
            rle.write_bytes(bytes([0, 0, 10]) + bytes(15))
            with self.assertRaises(ValueError):
                sweep.Frame(rle)

    def test_exercise_releases_every_button_it_presses(self):
        held = set()
        for command, delay in sweep.EXERCISE:
            self.assertGreaterEqual(delay, 0)
            if command.startswith("+"):
                held.add(command[1:])
            elif command.startswith("-"):
                self.assertIn(command[1:], held, command)
                held.discard(command[1:])
        self.assertEqual(held, set())


class SweepRunTests(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix="kisak-mp-sweep-test-")
        self.addCleanup(self._temporary.cleanup)
        root = Path(self._temporary.name)
        self.game = root / "game"
        (self.game / "main").mkdir(parents=True)
        (self.game / "zone" / "english").mkdir(parents=True)
        for name in ("mp_crash", "mp_backlot", "mp_bog"):
            (self.game / "zone" / "english" / f"{name}.ff").touch()
        self.binary = root / "fake-KisakCOD-mp"
        self.binary.write_text(FAKE_GAME.format(python=sys.executable))
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IXUSR)
        self.work = root / "work"
        self.saved_env = dict(os.environ)
        self.addCleanup(self.restore_env)
        self.saved_exercise = sweep.EXERCISE
        sweep.EXERCISE = SHORT_EXERCISE

    def restore_env(self):
        os.environ.clear()
        os.environ.update(self.saved_env)
        sweep.EXERCISE = self.saved_exercise

    def run_sweep(self, *extra):
        argv = [str(self.game), "--binary", str(self.binary), "--hold", "1", "--load-timeout", "10",
                "--probe-timeout", "3", "--quit-timeout", "5", "--menu-wait", "0.2", "--poll", "0.1",
                "--work-dir", str(self.work), *extra]
        return sweep.main(argv), (self.work / "report.md").read_text()

    def test_maps_plan_passes_and_skips_missing_zones(self):
        status, report = self.run_sweep("--maps", "mp_crash,mp_backlot,mp_vacant", "--screenshot", "--sound-debug")
        self.assertEqual(status, 0, report)
        self.assertRegex(report, r"\| mp_crash \| all steps seen \| \d+\.\d s \| 0 \| 120 \| exit 0 \| 0 \| 0 \| 0 \| 0 \|")
        self.assertIn("| mp_crash | 64x32 | 120 | 92% | 17 | 38% | #4080e0 | #787878 | #787878 |", report)
        self.assertIn("| mp_backlot | all steps seen |", report)
        self.assertIn("Maps without a zone file (not run): mp_vacant", report)
        self.assertIn("| mp_crash | 12 | 1 | 0 | 0 | 31 ms |", report)
        self.assertIn("Menus answered (name and position): team_marinesopfor (8), changeclass_mw (15).", report)
        self.assertTrue((self.work / "mp_crash.log").is_file())
        self.assertTrue((self.work / "home" / "main" / "screenshots" / "sweep_mp_crash.tga").is_file())
        status, report = self.run_sweep("--maps", "mp_crash", "--screenshot", "--delete-screenshots", "--no-exercise")
        self.assertEqual(status, 0, report)
        self.assertIn("| mp_crash | 64x32 | 120 |", report)
        self.assertFalse((self.work / "home" / "main" / "screenshots" / "sweep_mp_crash.tga").exists())
        self.assertEqual((self.work / "home" / "players" / "profiles" / "active.txt").read_text(), "sweep\n")
        self.assertTrue((self.work / "base" / "main").is_symlink())
        self.assertEqual(sorted(path.name for path in self.game.rglob("*")),
                         ["english", "main", "mp_backlot.ff", "mp_bog.ff", "mp_crash.ff", "zone"],
                         "nothing was written into the game directory")

    def test_chain_plan_finds_moved_menus_by_name(self):
        os.environ["FAKE_MOVED_MENUS_MAP"] = "mp_backlot"
        status, report = self.run_sweep("--plan", "chain", "--maps", "mp_crash,mp_backlot,mp_bog", "--no-exercise")
        self.assertEqual(status, 0, report)
        for name in ("mp_crash", "mp_backlot", "mp_bog"):
            self.assertIn(f"| {name} | all steps seen |", report)
        self.assertIn("team_marinesopfor (10), changeclass_mw (17)", report)
        self.assertIn("One process ran every map", report)

    def test_first_load_that_drops_is_asked_again(self):
        os.environ["FAKE_DROP_MAP"] = "mp_crash"
        status, report = self.run_sweep("--maps", "mp_crash", "--no-exercise")
        self.assertEqual(status, 0, report)
        self.assertRegex(report, r"\| mp_crash \| all steps seen \| \d+\.\d s \| 1 \| - \| exit 0 \| 0 \| 1 \|")
        self.assertIn("- `ERROR: PLATFORM_STATSREADERROR`", report)
        self.assertIn("after one drop to the menu", report)

    def test_join_that_never_spawns_is_reported(self):
        os.environ["FAKE_NO_SPAWN_MAP"] = "mp_backlot"
        status, report = self.run_sweep("--maps", "mp_crash,mp_backlot", "--no-exercise")
        self.assertEqual(status, 1)
        self.assertIn("| mp_crash | all steps seen |", report)
        self.assertIn("| mp_backlot | stopped at: player was alive |", report)
        self.assertIn("NOT SEEN: player was alive (no death logged 3 s after kill)", report)
        self.assertIn("- ok: clean exit (exit 0)", report)

    def test_wrong_menu_position_is_reported(self):
        status, report = self.run_sweep("--maps", "mp_crash", "--no-exercise", "--class-menu", "14")
        self.assertEqual(status, 1)
        self.assertIn("| mp_crash | stopped at: menus read |", report)
        self.assertIn("no menu 14 among 3 script menus", report)

    def test_menu_given_by_name(self):
        status, report = self.run_sweep("--maps", "mp_crash", "--no-exercise", "--team-menu", "team_marinesopfor",
                                        "--class-menu", "changeclass_mw")
        self.assertEqual(status, 0, report)

    def test_flat_frame_and_script_error_are_reported(self):
        os.environ["FAKE_FLAT_MAP"] = "mp_crash"
        os.environ["FAKE_SCRIPT_ERROR_MAP"] = "mp_crash"
        status, report = self.run_sweep("--maps", "mp_crash", "--screenshot")
        self.assertEqual(status, 1)
        self.assertRegex(report, r"\| mp_crash \| stopped at: frame drawn \| \d+\.\d s \| 0 \| 0 \| exit 0 \| 0 \| 0 \| 1 \|")
        self.assertIn("NOT SEEN: frame drawn (mean brightness 0 of 255, 1 colours, one colour on 100% of it)", report)
        self.assertIn("| mp_crash | 64x32 | 0 | 0% | 1 | 100% | #000000 | #000000 | #000000 |", report)
        self.assertIn("- ok: player was alive", report)
        self.assertIn("Script errors: 1", report)

    def test_crash_and_assertion_are_reported(self):
        os.environ["FAKE_CRASH_MAP"] = "mp_backlot"
        os.environ["FAKE_ASSERT_MAP"] = "mp_backlot"
        status, report = self.run_sweep("--maps", "mp_crash,mp_backlot")
        self.assertEqual(status, 1)
        self.assertIn("| mp_crash | all steps seen |", report)
        self.assertRegex(report, r"\| mp_backlot \| stopped at: joined \| \d+\.\d s \| 0 \| - \| signal 11 \| 1 \|")
        self.assertIn("- `ent->health > 0`", report)

    def test_map_that_never_loads_stops_the_chain(self):
        os.environ["FAKE_NOLOAD_MAP"] = "mp_backlot"
        status, report = self.run_sweep("--plan", "chain", "--maps", "mp_crash,mp_backlot,mp_bog", "--no-exercise",
                                        "--load-timeout", "2")
        self.assertEqual(status, 1)
        self.assertIn("| mp_crash | all steps seen |", report)
        self.assertIn("| mp_backlot | stopped at: in game |", report)
        self.assertIn("no 'state: 9' in 2 s", report)
        self.assertIn("| mp_bog | stopped at: reached |", report)

    def test_hold_shorter_than_the_exercise_is_refused(self):
        sweep.EXERCISE = self.saved_exercise
        with self.assertRaises(SystemExit) as refused:
            sweep.main([str(self.game), "--binary", str(self.binary), "--hold", "5", "--work-dir", str(self.work)])
        self.assertEqual(refused.exception.code, 2)
        self.assertFalse(self.work.exists())


if __name__ == "__main__":
    unittest.main()
