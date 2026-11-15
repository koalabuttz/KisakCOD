#!/usr/bin/env python3
"""Check scripts/runs/pc_run.py on the host with a stand-in game.

The stand-in (tests/runs/fake_pc_game.py) reads the timed commands pc_run.py
puts on its command line, writes its console log and its screenshots (plain
and RLE-packed TGAs, bottom row first) into the home folder pc_run.py gives
it, and exits as told. The test checks the command line, the recipe checks,
the log lines, the screenshots it collects, the picture numbers against
values worked out by hand, the PNGs, the checksum, a launch that fails, a
launch that hangs, and --resume.

No game, no GPU. Usage:
    python3 scripts/tests/test_pc_run.py
"""

import hashlib
import json
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "runs" / "pc_run.py"
FAKE = ROOT / "tests" / "runs" / "fake_pc_game.py"
sys.path.insert(0, str(RUNNER.parent))
import pc_run  # noqa: E402

FAILURES = []


def check(condition, what):
    if not condition:
        FAILURES.append(what)
        print(f"FAIL: {what}")


def recipe(work, launches, **extra):
    template = work / "home-template"
    (template / "main").mkdir(parents=True, exist_ok=True)
    (template / "main" / "config_mp.cfg").write_text("// template\n")
    value = {
        "format": "kisak-pc-run/1",
        "id": "test",
        "title": "pc_run test",
        "game": {"exe": [sys.executable, str(FAKE)], "cwd": str(work), "args": ["+set", "logfile", "2"],
                 "env": {"KISAK_VK_MODERN_EXPOSURE": "0"}, "clear_env": ["KISAK_VK_*"],
                 "home_template": str(template), "timeout": 20},
        "launches": launches,
        "lines": ["modern:", "device destroyed after"],
    }
    value.update(extra)
    path = work / "recipe.json"
    path.write_text(json.dumps(value))
    return path


def run(path, out, *more):
    return subprocess.run([sys.executable, str(RUNNER), str(path), "--out", str(out), *more],
                          capture_output=True, text=True, timeout=120)


def png_size(path):
    data = path.read_bytes()
    check(data[:8] == b"\x89PNG\r\n\x1a\n", f"{path.name} is a PNG")
    width, height = int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
    # The image data inflates to (1 + 3 * width) * height bytes.
    start = data.index(b"IDAT") + 4
    length = int.from_bytes(data[start - 8:start - 4], "big")
    check(len(zlib.decompress(data[start:start + length])) == (1 + 3 * width) * height, f"{path.name} inflates")
    return width, height


def test_recipe_checks(work):
    bad = [
        ([{"name": "a b"}], "launch name"),
        ([{"name": "a"}, {"name": "a"}], "twice"),
        ([{"name": "a", "at_map": ["soon quit"]}], "not '<seconds> <command>'"),
        ([{"name": "a", "at_map": ["1 echo x | 2 quit"]}], "without '|'"),
        ([{"name": "a", "at_map": ["1 screenshot s"]}, {"name": "b", "at_map": ["1 screenshot s"]}], "named twice"),
        ([{"name": "a", "at_map": ["45 +left", "50 -left"]}], "has a '+'"),
        ([{"name": "a", "args": ["+set", "x", "1"] * 30, "at_map": ["1 quit"]}], "'+' parts"),
    ]
    for launches, message in bad:
        result = run(recipe(work, launches), work / "out-bad")
        check(result.returncode == 2 and message in result.stderr, f"recipe refused: {message} ({result.stderr.strip()})")
    path = recipe(work, [{"name": "a", "at_map": ["1 screenshot s"]}],
                  pictures={"compare": [{"a": "s", "b": "nope"}]})
    result = run(path, work / "out-bad")
    check(result.returncode == 2 and "'nope'" in result.stderr, "compare of an unknown screenshot refused")


def test_command_line(work):
    path = recipe(work, [{"name": "v", "args": ["+devmap", "mp_crash"],
                          "at_map": ["5 setviewpos 1 2 3 4", "21 screenshot v-1", "23 quit"]}])
    rec = pc_run.load_recipe(path)
    argv = pc_run.launch_argv(rec, rec["launches"][0], Path("H"))
    check(argv[-2:] == ["+devmap", "mp_crash"], "the launch's own arguments come last")
    check(["+set", "run_atMap", "5 setviewpos 1 2 3 4 | 21 screenshot v-1 | 23 quit"] == argv[argv.index("run_atMap") - 1:argv.index("run_atMap") + 2],
          "run_atMap is the joined list")
    check(argv[argv.index("run_quitAfterMap") + 1] == "53", "safety quit 30 s after the last command")
    check(argv[argv.index("fs_homepath") + 1] == "H", "fs_homepath is the launch's home")
    result = run(path, work / "out-dry", "--dry-run")
    check(result.returncode == 0 and "run_atMap" in result.stdout, "dry run prints the launches")
    check(not (work / "out-dry" / "v").exists(), "dry run starts nothing")


def test_full_run(work):
    launches = [
        {"name": "base", "env": {"KISAK_VK_MODERN_AO": "0"},
         "at_map": ["5 framelog 10 base", "16 screenshot shot-B", "18 quit"]},
        {"name": "lit", "env": {"FAKE_SQUARE": "180", "FAKE_RLE": "1"},
         "at_map": ["5 framelog 10 lit", "16 screenshot shot-L", "18 quit"]},
    ]
    path = recipe(work, launches, pictures={"regions": {"square": [100, 60, 164, 124], "left": [0, 0, 50, 200]},
                                            "levels": [2, 16], "compare": [{"a": "shot-B", "b": "shot-L"}]})
    out = work / "out"
    result = run(path, out)
    check(result.returncode == 0, f"full run exits 0 ({result.stdout} {result.stderr})")
    results = json.loads((out / "results.json").read_text())
    body = {key: value for key, value in results.items() if key != "sha256"}
    check(results["sha256"] == hashlib.sha256(pc_run.canonical(body).encode("ascii")).hexdigest(), "checksum")
    base, lit = results["launches"]
    check(base["status"] == "ok" and base["exit_code"] == 0, "base launch ok")
    check(base["frame_logs"] and base["frame_logs"][0].startswith("frame log base: 1000 frames"), "frame log line kept")
    check(any("device destroyed after 0 validation errors" in line for line in base["lines"]), "wanted line kept")
    check(not any("KISAK_VK_MODERN_EXPOSURE" in line for line in base["lines"]), "only wanted lines")
    check(list(base["shots"]) == ["shot-B"] and not base["missing_shots"], "screenshot collected")
    check((out / "base" / "console_mp.log").exists(), "console log copied")
    log = (out / "base" / "console_mp.log").read_text()
    check("KISAK_VK_MODERN_AO=0" in log, "launch environment reaches the game")
    check("KISAK_NO_DIALOGS=1" in log, "every launch answers the game's questions itself (KISAK_NO_DIALOGS)")
    check("KISAK_VK_MODERN_AO=-" in (out / "lit" / "console_mp.log").read_text(), "clear_env removes old switches")

    pictures = results["pictures"]
    check(pictures["means"]["shot-B"]["whole"] == [100.0, 100.0, 100.0], "mean of the plain picture")
    check(pictures["means"]["shot-L"]["square"] == [180.0, 180.0, 180.0], "mean of a region (RLE, bottom row first)")
    check(pictures["means"]["shot-L"]["left"] == [100.0, 100.0, 100.0], "region outside the square unchanged")
    pair = pictures["compare"][0]
    square_share = 64 * 64 / (320 * 200)
    check(abs(pair["whole"]["share_over"]["16"] - round(square_share, 4)) < 1e-4, "share over 16 levels")
    check(abs(pair["whole"]["mean_abs"] - round(80 * square_share, 2)) < 0.011, "mean |diff|")
    check(pair["whole"]["mean_signed_b_minus_a"] > 0, "b brighter than a")
    check(pair["square"]["share_over"]["2"] == 1.0, "every pixel of the square differs")
    check(pair["left"]["mean_abs"] == 0, "nothing differs in the left strip")
    boxes = pair["whole"]["boxes_over_top"]
    check(len(boxes) == 1 and boxes[0]["box"] == [96, 32, 192, 128] and boxes[0]["pixels"] == 4096,
          f"one changed area around the square ({boxes})")
    check(png_size(out / "pictures" / "pair-shot-B-vs-shot-L.png") == (160 + 8 + 160, 100), "pair is half size")
    check(png_size(out / "pictures" / "diff-shot-B-vs-shot-L.png") == (320, 200), "diff is full size")
    check(png_size(out / "pictures" / "crop-shot-B-vs-shot-L.png") == (160 + 8 + 160, 160), "crop around the change")
    report = (out / "report.md").read_text()
    check("| base | ok | 0 |" in report and "shot-B vs shot-L" in report and results["sha256"] in report, "report")

    # --resume runs nothing that already passed.
    stamp = (out / "base" / "console_mp.log").stat().st_mtime
    result = run(path, out, "--resume")
    check(result.returncode == 0 and "base ..." not in result.stdout, "resume skips launches that passed")
    check((out / "base" / "console_mp.log").stat().st_mtime == stamp, "resume leaves their files")


def test_variants(work):
    launch = {"name": "live", "at_map": ["3 setviewpos 1 2 3 4"],
              "variants": [{"name": "B", "set": {"r_fakeCost": "0", "r_fakeSquare": "100"}},
                           {"name": "A", "set": {"r_fakeCost": "2.5", "r_fakeSquare": "180"}}],
              "variant_timing": {"start": 5, "settle": 2, "framelog": 4, "warmup": 0, "compare": True}}
    path = recipe(work, [launch], pictures={"regions": {"square": [100, 60, 164, 124]}})
    commands = pc_run.launch_commands(pc_run.load_recipe(path)["launches"][0], "at_map")
    check(commands == ["3 setviewpos 1 2 3 4",
                       "5 r_fakeCost 0; r_fakeSquare 100", "7 screenshot live-B", "8 framelog 4 live-B-1",
                       "13 r_fakeCost 2.5; r_fakeSquare 180", "15 screenshot live-A", "16 framelog 4 live-A-1",
                       "21 r_fakeCost 2.5; r_fakeSquare 180", "23 framelog 4 live-A-2",
                       "28 r_fakeCost 0; r_fakeSquare 100", "30 framelog 4 live-B-2", "35 quit"],
          f"variants become timed commands, A B B A, one screenshot each ({commands})")
    plain = pc_run.launch_commands({"name": "p", "at_map": ["3 echo x"], "variants": [{"name": "v", "set": {"r_a": "1"}}]}, "at_map")
    check(plain[1] == "20 r_a 1", f"variants start after the pre-match countdown by default ({plain})")
    timed = pc_run.launch_commands({"name": "p", "at_map": ["3 echo x"], "variant_timing": {"framelog": 5},
                                    "variants": [{"name": "v", "set": {"r_a": "1"}}, {"name": "w", "set": {"r_a": "2"}}]},
                                   "at_map")
    check(timed[1:5] == ["20 r_a 1", "51 screenshot p-v", "52 framelog 5 p-v-1", "58 r_a 2"],
          f"settle 1 s, and 30 s of warm-up before the first frame log ({timed})")
    late = pc_run.launch_commands({"name": "p", "at_map": ["25 echo x"], "variants": [{"name": "v", "set": {"r_a": "1"}}]}, "at_map")
    check(late[1] == "27 r_a 1", f"or 2 s after a later at_map command ({late})")
    out = work / "out-variants"
    result = run(path, out)
    check(result.returncode == 0, f"variant run exits 0 ({result.stdout} {result.stderr})")
    results = json.loads((out / "results.json").read_text())
    live = results["launches"][0]
    check(sorted(live["shots"]) == ["live-A", "live-B"] and not live["missing_shots"], "one screenshot per variant")
    base, lit = live["variants"]
    check([p["label"] for p in base["passes"]] == ["live-B-1", "live-B-2"], "B's frame logs, both passes")
    check(base["mean_fps"] == 100.0 and lit["mean_fps"] == 80.0, f"mean fps per variant ({base}, {lit})")
    check(lit["fps_vs_first_pct"] == -20.0 and lit["ms_vs_first"] == 2.5, "A against B: -20%, +2.5 ms")
    check(base.get("pass_spread_pct") == 0.0, "pass spread")
    pair = results["pictures"]["compare"][0]
    check(pair["a"] == "live-B" and pair["b"] == "live-A" and pair["label"] == "live: B vs A", "compare made itself")
    check(pair["square"]["mean_abs"] == 80.0, "the square changed with the variant")
    report = (out / "report.md").read_text()
    check("| A | r_fakeCost 2.5, r_fakeSquare 180 | 80.0, 80.0 | 80.0 | -20.00% | +2.500 | 0.0% |" in report,
          "variant table in the report")

    bad = [
        ({"variants": []}, "variants is a list"),
        ({"variants": [{"name": "x", "set": {}}]}, "sets nothing"),
        ({"variants": [{"name": "x", "set": {"r_a": "1; quit"}}]}, "one value without spaces"),
        ({"variants": [{"name": "x", "set": {"r_a": "+1"}}]}, "one value without spaces"),
        ({"variants": [{"name": "x", "set": {"r_a": "1"}}, {"name": "x", "set": {"r_a": "2"}}]}, "given twice"),
        ({"variants": [{"name": "x", "set": {"r_a": "1"}}], "variant_timing": {"start": 3}}, "not after the last"),
        ({"variants": [{"name": "x", "set": {"r_a": "1"}}], "variant_timing": {"passes": 3}}, "passes is 1 or 2"),
        ({"variants": [{"name": "x", "set": {"r_a": "1"}}], "variant_timing": {"pass": 1}}, "unknown key"),
        ({"variants": [{"name": f"v{i}", "set": {"r_a": str(i)}} for i in range(10)],
          "variant_timing": {"framelog": 2}}, "the client takes 32"),
    ]
    for extra, message in bad:
        result = run(recipe(work, [{"name": "a", "at_map": ["3 echo x"], **extra}]), work / "out-bad")
        check(result.returncode == 2 and message in result.stderr, f"variants refused: {message} ({result.stderr.strip()})")


def test_failures(work):
    launches = [
        {"name": "fails", "env": {"FAKE_EXIT": "3"}, "at_map": ["1 screenshot f-1", "2 quit"]},
        {"name": "noshot", "at_map": ["1 screenshot", "2 quit"]},
        {"name": "hangs", "env": {"FAKE_HANG": "1"}, "timeout": 2, "at_map": ["1 screenshot h-1"]},
        {"name": "refused", "env": {"FAKE_REFUSE_LIST": "1"}, "at_map": ["1 echo x", "2 quit"]},
    ]
    out = work / "out-fail"
    result = run(recipe(work, launches), out)
    check(result.returncode == 1, "a failed launch makes the run fail")
    results = {launch["name"]: launch for launch in json.loads((out / "results.json").read_text())["launches"]}
    check(results["fails"]["status"] == "exit" and results["fails"]["exit_code"] == 3, "exit code kept")
    check(results["hangs"]["status"] == "timeout", "a hung launch is ended")
    check(results["hangs"]["missing_shots"] == ["h-1"], "its screenshot is missing")
    check(results["refused"]["status"] == "run_error" and results["refused"]["exit_code"] == 0
          and results["refused"]["run_errors"], "a refused timed list fails the launch though the game exits 0")


def main():
    with tempfile.TemporaryDirectory(prefix="kisak-pc-run-") as name:
        work = Path(name)
        test_recipe_checks(work)
        test_command_line(work)
        test_full_run(work)
        test_variants(work)
        test_failures(work)
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} pc_run check(s)")
        return 1
    print("PASS: pc_run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
