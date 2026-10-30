#!/usr/bin/env python3
"""Opt-in native startup/unload check using a legally installed COD4 asset tree.

Only symlinks to main/zone/localization.txt are staged. Writable files go to a
separate temporary home, and UDP is disabled. Optional map validation runs server
frames, requests status, and quits; it does not exercise client connections.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ZONES = ("code_post_gfx_mp", "localized_code_post_gfx_mp", "common_mp", "localized_common_mp")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_dir", type=Path, help="COD4 Steam installation directory")
    parser.add_argument("--binary", type=Path, default=ROOT / "bin/KisakCOD-dedi")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--map", dest="map_name", help="also initialize a map, run frames, and check status")
    args = parser.parse_args()
    game = args.game_dir.expanduser().resolve()
    binary = args.binary.expanduser().resolve()
    for name in ("main", "zone", "localization.txt"):
        if not (game / name).exists():
            parser.error(f"missing installed asset path: {game / name}")
    if not binary.is_file():
        parser.error(f"missing native executable: {binary}")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.map_name and not all(ch.isascii() and (ch.isalnum() or ch == "_") for ch in args.map_name):
        parser.error("--map must contain only ASCII letters, digits, and underscores")
    with tempfile.TemporaryDirectory(prefix="kisak-linux-assets-") as temporary:
        work = Path(temporary)
        assets, home = work / "assets", work / "home"
        assets.mkdir()
        home.mkdir()
        for name in ("main", "zone"):
            (assets / name).symlink_to(game / name, target_is_directory=True)
        (home / "localization.txt").symlink_to(game / "localization.txt")
        command = [str(binary), "+set", "fs_basepath", str(assets),
                   "+set", "fs_homepath", str(home), "+set", "net_noudp", "1",
                   "+set", "net_ip", "127.0.0.1", "+set", "dedicated", "1"]
        if args.map_name:
            command += ["+map", args.map_name]
            console_input = "wait 200\nstatus\nquit\n"
        else:
            command += ["+quit"]
            console_input = ""
        try:
            completed = subprocess.run(command, cwd=home, input=console_input,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, errors="replace", timeout=args.timeout)
        except subprocess.TimeoutExpired as error:
            print(f"FAIL: asset startup/unload timed out after {args.timeout}s")
            if error.stdout:
                print(error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else error.stdout)
            return 1
        output = completed.stdout
        zones = (*ZONES, args.map_name) if args.map_name else ZONES
        checks = [(completed.returncode == 0, f"startup/quit exits 0 (got {completed.returncode})"),
                  ("Common Initialization Complete" in output, "real common initialization completes"),
                  ((home / "main/console_mp.log").is_file(), "log stays in the separate writable home")]
        checks.extend((f"Loaded zone '{zone}'" in output, f"loads {zone}") for zone in zones)
        checks.extend((f"Unloaded fastfile {zone}" in output, f"unloads {zone}") for zone in zones)
        if args.map_name:
            checks += [("Game Initialization" in output, "reaches game initialization"),
                       ("Dedicated server initialized" in output, "map initialization returns to the main loop"),
                       (f"map: {args.map_name}" in output, "status reports the active map after running frames"),
                       ("ShutdownGame" in output, "runs game shutdown"),
                       ("ASSERT FAILED" not in output and "FATAL ERROR" not in output,
                        "no assertion or fatal error during map startup/shutdown")]
        for ok, message in checks:
            print(f"{'PASS' if ok else 'FAIL'}: {message}")
        if not all(ok for ok, _ in checks):
            print("\nEngine output:\n" + output)
            return 1
    if args.map_name:
        print("Native map startup/frames/status/shutdown passed; client connections and gameplay remain untested.")
    else:
        print("Native asset startup/unload passed; map loading and gameplay are separate checks.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
