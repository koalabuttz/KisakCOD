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
        "args": ["+set", "r_fullscreen", "1", "+set", "r_mode", "1280x720",
                 "+set", "snd_volume", "0", "+set", "logfile", "2"],
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
         "at_map": ["3 cmd mr {serverid} 8 autoassign", "5 setviewpos 30 -10 200 140",
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
``forbid_processes`` are checked before and after every launch (Windows:
tasklist); a launch never starts while one runs, nor while another copy of
the game does.

Exit status: 0 when every launch exited 0 and wrote every screenshot it
named, 1 otherwise (the report says which), 2 on a bad recipe.
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
SAFETY_AFTER_LAST = 30  # seconds after the last timed command before the safety quit
FRAME_LOG = re.compile(r"^frame log (\S+): .*$")
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
        for key in ("at_menu", "at_map"):
            commands = launch.get(key) or []
            if len(commands) > 32:
                raise RecipeError(f"{name}: {key} has {len(commands)} commands; the client takes 32")
            for command in commands:
                if "|" in command or not TIMED.match(command):
                    raise RecipeError(f"{name}: {key} entry {command!r} is not '<seconds> <command>' without '|'")
                if len(command.split(None, 1)[1]) >= 160:
                    raise RecipeError(f"{name}: {key} entry {command!r} is longer than the client takes")
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


def launch_shots(launch):
    shots = []
    for command in (launch.get("at_menu") or []) + (launch.get("at_map") or []):
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
    at_menu, at_map = launch.get("at_menu") or [], launch.get("at_map") or []
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


def pictures(recipe, results, out):
    settings = recipe.get("pictures") or {}
    levels = sorted(settings.get("levels") or [2, 16])
    regions = settings.get("regions") or {}
    shots = {name: path for launch in results for name, path in (launch.get("shots") or {}).items()}
    cache = {}

    def image(name):
        if name not in cache:
            cache[name] = read_tga(shots[name])
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
    for pair in settings.get("compare") or []:
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
        verbatim = (launch.get("frame_logs") or []) + (launch.get("lines") or [])
        if verbatim:
            lines += ["```"] + verbatim + ["```"]
    pics = results.get("pictures") or {}
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
