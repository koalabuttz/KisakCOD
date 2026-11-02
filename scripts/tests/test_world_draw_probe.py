#!/usr/bin/env python3
"""Fail-closed check for the world draw probe without a D3D9 implementation.

The world draw probe (scripts/platform/linux/world_draw_probe.cmake) is the
offline asset exporter with the whole renderer. CI links it against
tests/linux_device_probe/d3d9_unavailable.cpp, whose Direct3DCreate9 answers like
DXVK Native's does without a Vulkan device, and this script runs it eight times.
In the draw mode (KISAK_WORLD_DRAW_MAP):

  * with an allowlisted map: the engine starts, the layout check passes, and the
    probe must stop at its own Direct3DCreate9 check, before R_Init runs (the
    engine's R_InitGraphicsApi would go on without an interface);
  * with no map: refused before any engine work;
  * with a map outside the allowlist: refused before any device work;
  * with an allowlisted map and a show time that is not a whole number of
    seconds from 0 to 60: refused with the other arguments, before any device
    work.

In the free-camera replay (KISAK_FREE_CAMERA_MAP):

  * with an allowlisted map: the same stop at Direct3DCreate9, named by the
    replay;
  * with both map variables set: refused before any engine work;
  * with a map outside the allowlist: refused before any device work;
  * with a show time, which belongs to the draw mode: refused with the other
    arguments, before any device work.

Each run exits 1 and writes nothing to its private output directory.

No game data is involved. The engine refuses to start unless fileSysCheck.cfg is
in its base path, so the script writes a one-line stub into a temporary tree it
creates and removes; that file is the only one in the tree. Nothing else is read
from or written outside that tree, and the script takes no game directory.

It is meant for the stand-in library. With a working DXVK library and a Vulkan
device the probe gets past Direct3DCreate9 and the first run fails differently.

    LD_LIBRARY_PATH=<SDL3 lib>:<stand-in dir> SDL_VIDEODRIVER=dummy \\
      python3 scripts/tests/test_world_draw_probe.py --probe out/KisakCOD-world-draw-probe
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ENGINE_START = "asset_export=offline database=engine networking=not-initialized renderer=draw"
CAMERA_START = "asset_export=offline database=engine networking=not-initialized renderer=free_camera"
DEVICE_FAILURE = "Offline asset export failed: world_draw_probe stage=direct3d: "
CAMERA_DEVICE_FAILURE = "Offline asset export failed: free_camera_replay stage=direct3d: "
NO_MAP = ("Offline asset export failed: KisakCOD-world-draw-probe needs KISAK_WORLD_DRAW_MAP=<map> or "
          "KISAK_FREE_CAMERA_MAP=<map>")
BOTH_MAPS = "Offline asset export failed: KISAK_WORLD_DRAW_MAP and KISAK_FREE_CAMERA_MAP are mutually exclusive"
UNKNOWN_MAP = "Offline asset export failed: world_draw_probe stage=arguments: "
CAMERA_ARGUMENTS = "Offline asset export failed: free_camera_replay stage=arguments: "
# Every export mode the exporter reads; a stray one in the caller's environment
# would change which refusal the probe gives.
MODE_VARIABLES = ("KISAK_WORLD_DRAW_MAP", "KISAK_FREE_CAMERA_MAP", "KISAK_WORLD_DEVICE_MAP",
                  "KISAK_WORLD_SURFACE_MAP", "KISAK_WORLD_FRONTEND_MAP", "KISAK_UI_ASSET_EXPORT_MENUS",
                  "KISAK_UI_ASSET_EXPORT_DIR", "KISAK_WORLD_DRAW_SHOW_SECONDS")


def run_probe(probe, tree, output, map_name, timeout, show_seconds=None, camera_map=None):
    environment = {key: value for key, value in os.environ.items() if key not in MODE_VARIABLES}
    environment["KISAK_UI_ASSET_EXPORT_DIR"] = str(output)
    if map_name is not None:
        environment["KISAK_WORLD_DRAW_MAP"] = map_name
    if camera_map is not None:
        environment["KISAK_FREE_CAMERA_MAP"] = camera_map
    if show_seconds is not None:
        environment["KISAK_WORLD_DRAW_SHOW_SECONDS"] = show_seconds
    command = [str(probe), "+set", "fs_basepath", str(tree / "base"), "+set", "fs_homepath", str(tree / "home"),
               "+set", "dedicated", "2", "+set", "useFastFile", "1"]
    print("$ " + " ".join(command), flush=True)
    try:
        completed = subprocess.run(command, cwd=tree, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output_text = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
        print(output_text)
        return None, output_text
    print(completed.stdout, flush=True)
    return completed.returncode, completed.stdout


def last_line(text):
    lines = text.rstrip("\n").splitlines()
    return lines[-1] if lines else ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe", type=Path, required=True, help="the KisakCOD-world-draw-probe executable")
    parser.add_argument("--timeout", type=int, default=120, help="seconds per run")
    args = parser.parse_args()
    probe = args.probe.expanduser().resolve()
    if not probe.is_file():
        parser.error(f"missing probe executable: {probe}")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    checks = []

    def check(ok, message):
        checks.append((ok, message))
        print(f"{'PASS' if ok else 'FAIL'}: {message}", flush=True)

    with tempfile.TemporaryDirectory(prefix="kisak-world-draw-") as temporary:
        tree = Path(temporary)
        (tree / "base" / "main").mkdir(parents=True)
        (tree / "home").mkdir()
        (tree / "base" / "main" / "fileSysCheck.cfg").write_text("// stub written by the check; no game data\n")
        outputs = tree / "out"
        outputs.mkdir(mode=0o700)

        status, text = run_probe(probe, tree, outputs / "mp_crash", "mp_crash", args.timeout)
        check(status == 1, f"an allowlisted map without a device exits 1 (got {status})")
        lines = text.splitlines()
        check(ENGINE_START in lines, "the engine starts in draw mode")
        check(any(line.startswith("world_draw_layout=agreed types=") for line in lines),
              "the probe and the loader agree on the shared layouts")
        check(last_line(text).startswith(DEVICE_FAILURE), "the last line names the direct3d stage")
        check(not any(line.startswith("world_draw_renderer=starting") for line in lines),
              "R_Init does not run without a Direct3D 9 interface")
        check("Getting Direct3D 9 interface..." not in lines, "the engine's own device start is never reached")

        status, text = run_probe(probe, tree, outputs / "unset", None, args.timeout)
        check(status == 1, f"no map exits 1 (got {status})")
        check(last_line(text) == NO_MAP, "no map is refused by name")
        check(ENGINE_START not in text.splitlines(), "no map is refused before the draw mode starts")

        status, text = run_probe(probe, tree, outputs / "unknown", "mp_not_a_map", args.timeout)
        check(status == 1, f"a map outside the allowlist exits 1 (got {status})")
        check(last_line(text).startswith(UNKNOWN_MAP), "a map outside the allowlist is refused at the arguments stage")
        check(not any(line.startswith("direct3d=") for line in text.splitlines()),
              "a refused map is refused before any device work")

        status, text = run_probe(probe, tree, outputs / "show", "mp_crash", args.timeout, show_seconds="61")
        check(status == 1, f"a show time over 60 seconds exits 1 (got {status})")
        check(last_line(text).startswith(UNKNOWN_MAP + "KISAK_WORLD_DRAW_SHOW_SECONDS"),
              "a show time over 60 seconds is refused at the arguments stage")
        check(not any(line.startswith("world_draw_layout=") for line in text.splitlines()),
              "a bad show time is refused before the layout check and any device work")

        # The free-camera replay: the same start, the same refusals, named by the replay.
        status, text = run_probe(probe, tree, outputs / "camera", None, args.timeout, camera_map="mp_crash")
        check(status == 1, f"the replay with an allowlisted map and no device exits 1 (got {status})")
        lines = text.splitlines()
        check(CAMERA_START in lines, "the engine starts in free-camera mode")
        check(ENGINE_START not in lines, "the replay does not start the draw mode")
        check(any(line.startswith("world_draw_layout=agreed types=") for line in lines),
              "the replay's layouts agree with the loader's")
        check(last_line(text).startswith(CAMERA_DEVICE_FAILURE), "the replay's last line names the direct3d stage")
        check(not any(line.startswith("world_draw_renderer=starting") for line in lines),
              "the replay does not run R_Init without a Direct3D 9 interface")

        status, text = run_probe(probe, tree, outputs / "both", "mp_crash", args.timeout, camera_map="mp_crash")
        check(status == 1, f"both map variables exit 1 (got {status})")
        check(last_line(text) == BOTH_MAPS, "both map variables are refused by name")
        check(not any(line.startswith("asset_export=offline") for line in text.splitlines()),
              "both map variables are refused before either mode starts")

        status, text = run_probe(probe, tree, outputs / "camera-unknown", None, args.timeout,
                                 camera_map="mp_not_a_map")
        check(status == 1, f"the replay with a map outside the allowlist exits 1 (got {status})")
        check(last_line(text) == CAMERA_ARGUMENTS + "KISAK_FREE_CAMERA_MAP must name an allowlisted map",
              "the replay refuses a map outside the allowlist at the arguments stage")
        check(not any(line.startswith("direct3d=") for line in text.splitlines()),
              "the replay refuses that map before any device work")

        status, text = run_probe(probe, tree, outputs / "camera-show", None, args.timeout, show_seconds="5",
                                 camera_map="mp_crash")
        check(status == 1, f"the replay with a show time exits 1 (got {status})")
        check(last_line(text).startswith(CAMERA_ARGUMENTS + "KISAK_WORLD_DRAW_SHOW_SECONDS"),
              "the replay refuses a show time at the arguments stage")
        check(not any(line.startswith("world_draw_layout=") for line in text.splitlines()),
              "the replay refuses a show time before the layout check and any device work")

        written = sorted(str(path.relative_to(outputs)) for path in outputs.rglob("*") if path.is_file())
        check(not written, f"nothing is written to the private output directories ({written})")

    failed = [message for ok, message in checks if not ok]
    print(f"world_draw_probe fail-closed check: {len(checks) - len(failed)}/{len(checks)} passed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
