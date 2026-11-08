#!/usr/bin/env python3
"""Check the multiplayer flashbang white-out and concussion effect from screenshots.

Two steps, both run on the machine that has the game:

1. ``write-cfg <file>`` writes a config that, once the map is up, takes seven
   screenshots with the game's own ``screenshot`` command:

   - ``shk_none_a``, ``shk_none_b`` and (last) ``shk_none_c``: no shock, to show
     the view stays still for the whole run;
   - ``shk_flash_on`` / ``shk_flash_off``: ``cg_shellshock 4 flashbang`` with
     ``cg_drawShellshock`` 1 and 0;
   - ``shk_conc_on`` / ``shk_conc_off``: the same with ``concussion_grenade_mp``.

   ``cg_drawShellshock 0`` keeps the camera shake but skips the screen effect, so
   each on/off pair isolates the effect from everything else. The config only
   sets dvars: ``exec`` runs a file in one go and ignores ``wait``, so the steps
   run through ``vstr shk_go`` from the command line, where ``wait`` works.

   Launch the game with, for example::

       +exec shk.cfg +devmap mp_crash +wait 5000 +vstr shk_go

   On the Linux client, holding the command buffer with ``+wait`` lets the map
   connect before the start-up stats load and trips an assert in the stats
   upload. There, launch with ``+exec shk.cfg +devmap mp_crash`` only and send
   ``vstr shk_go`` through the console (stdin) once the map and stats are in.

   The game quits by itself when done. Nothing has to be spawned: the config
   closes the team menu and uses the still spectator view, and ``devmap``
   allows the cheat dvar.

2. ``check <screenshots dir>`` reads the seven TGA files and prints numbers and a
   verdict. Only numbers are printed; the images stay where they are.

Pass rules:

- flashbang: with the effect on, the frame is mostly white (mean luma at least
  200 and at least half the pixels above 240), and clearly brighter than the
  same shock with the effect off (by at least 40).
- concussion: the effect visibly changes the frame against the same shock
  with the effect off: edge strength (mean absolute luma gradient) at most
  0.85x, or mean luma moved by at least 30.
- both: the three no-shock frames (before and after the shocks) agree within
  0.95-1.05x in edge strength and mean luma (a still view).

The concussion "blur" blends the previous frame back in at about 99% weight
every frame. From a still view it smears little, and the 8-bit feedback loop
drifts brighter or darker depending on the GPU's blend rounding (seen: NVIDIA
on Windows brighter, Intel through DXVK darker). So the rule asks for a visible
change, and an information line gives fine-over-coarse edge strength, which
drops with real blur but not with a brightness change.

The numbers are computed over the whole frame and over its right half (the
team menu sits on the left); the verdict uses the right half.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys

SHOTS = ("shk_none_a", "shk_none_b", "shk_none_c", "shk_flash_on", "shk_flash_off", "shk_conc_on", "shk_conc_off")

# Wait counts are command buffer passes (about two per frame). At com_maxfps 85
# that is roughly 180 per second.
CFG_TEXT = """// Written by scripts/tools/shellshock_screenshot_check.py write-cfg.
// Launch: +exec shk.cfg +devmap mp_crash +wait 5000 +vstr shk_go
set shk_go "closemenu team_marinesopfor; wait 3000; screenshot shk_none_a; wait 120; screenshot shk_none_b; wait 60; vstr shk_flash"
set shk_flash "cg_drawShellshock 1; cg_shellshock 4 flashbang; wait 60; screenshot shk_flash_on; wait 1500; cg_drawShellshock 0; cg_shellshock 4 flashbang; wait 60; screenshot shk_flash_off; wait 1500; vstr shk_conc"
set shk_conc "cg_drawShellshock 1; cg_shellshock 4 concussion_grenade_mp; wait 120; screenshot shk_conc_on; wait 1500; cg_drawShellshock 0; cg_shellshock 4 concussion_grenade_mp; wait 120; screenshot shk_conc_off; wait 1500; vstr shk_end"
set shk_end "screenshot shk_none_c; cg_drawShellshock 1; quit"
"""

FLASH_MIN_MEAN = 200.0
FLASH_MIN_WHITE = 0.5
FLASH_MIN_GAIN = 40.0
BLUR_MAX_RATIO = 0.85
CONC_MIN_SHIFT = 30.0
COARSE = 4  # pixel step of the coarse difference
STILL_RANGE = (0.95, 1.05)


class Image:
    def __init__(self, width: int, height: int, luma: list[float]):
        self.width = width
        self.height = height
        self.luma = luma  # row-major, top row first


def read_tga(path: Path) -> Image:
    data = path.read_bytes()
    if len(data) < 18:
        raise ValueError(f"{path}: too short for a TGA header")
    id_len, cmap_type, img_type = data[0], data[1], data[2]
    width, height, bpp, desc = struct.unpack_from("<HHBB", data, 12)
    if cmap_type != 0 or img_type != 2 or bpp not in (24, 32):
        raise ValueError(f"{path}: only uncompressed 24/32-bit true-colour TGA is supported")
    step = bpp // 8
    offset = 18 + id_len
    need = offset + width * height * step
    if len(data) < need:
        raise ValueError(f"{path}: pixel data is truncated")
    top_first = bool(desc & 0x20)
    rows = []
    for row in range(height):
        start = offset + row * width * step
        px = data[start:start + width * step]
        # BGR(A) order.
        rows.append([0.114 * px[i] + 0.587 * px[i + 1] + 0.299 * px[i + 2] for i in range(0, len(px), step)])
    if not top_first:
        rows.reverse()
    return Image(width, height, [v for r in rows for v in r])


def write_tga(path: Path, width: int, height: int, gray: list[int]) -> None:
    """Writes a grey 24-bit top-first TGA (used by the self-test)."""
    header = bytes([0, 0, 2]) + bytes(9) + struct.pack("<HHBB", width, height, 24, 0x20)
    path.write_bytes(header + bytes(v for g in gray for v in (g, g, g)))


def metrics(img: Image, x0_frac: float = 0.0) -> dict[str, float]:
    w, h, l = img.width, img.height, img.luma
    x0 = int(w * x0_frac)
    total = 0.0
    coarse = 0.0
    ccount = 0
    white = 0
    grad = 0.0
    count = 0
    gcount = 0
    for y in range(h):
        base = y * w
        for x in range(x0, w):
            v = l[base + x]
            total += v
            if v > 240:
                white += 1
            count += 1
            if x + 1 < w and y + 1 < h:
                grad += abs(l[base + x + 1] - v) + abs(l[base + w + x] - v)
                gcount += 1
            if x + COARSE < w and y + COARSE < h:
                coarse += abs(l[base + x + COARSE] - v) + abs(l[base + COARSE * w + x] - v)
                ccount += 1
    edge = grad / gcount if gcount else 0.0
    coarse = coarse / ccount if ccount else 0.0
    return {
        "mean": total / count,
        "white": white / count,
        "edge": edge,
        # Fine over coarse differences: a blur lowers it, a plain brightness or
        # contrast change scales both and leaves it alone.
        "sharp": edge / coarse if coarse > 1e-6 else 0.0,
    }


def check(shot_dir: Path) -> int:
    missing = [n for n in SHOTS if not (shot_dir / f"{n}.tga").exists()]
    if missing:
        print("FAIL missing screenshots: " + ", ".join(missing))
        return 1
    full = {}
    right = {}
    for name in SHOTS:
        img = read_tga(shot_dir / f"{name}.tga")
        full[name] = metrics(img)
        right[name] = metrics(img, 0.5)
        print(f"{name:14s} {img.width}x{img.height}  "
              f"full: mean {full[name]['mean']:6.1f} white {full[name]['white']:.3f} edge {full[name]['edge']:6.2f} sharp {full[name]['sharp']:.3f}  "
              f"right half: mean {right[name]['mean']:6.1f} white {right[name]['white']:.3f} edge {right[name]['edge']:6.2f} sharp {right[name]['sharp']:.3f}")

    r = right
    ok = True
    still_ok = True
    for later in ("shk_none_b", "shk_none_c"):
        edge = r[later]["edge"] / max(r["shk_none_a"]["edge"], 1e-6)
        mean = r[later]["mean"] / max(r["shk_none_a"]["mean"], 1e-6)
        good = all(STILL_RANGE[0] <= v <= STILL_RANGE[1] for v in (edge, mean))
        still_ok &= good
        print(f"still view: {later[4:]}/none_a edge {edge:.3f} mean {mean:.3f} ({'ok' if good else 'MOVING'})")

    on, off = r["shk_flash_on"], r["shk_flash_off"]
    flash_ok = on["mean"] >= FLASH_MIN_MEAN and on["white"] >= FLASH_MIN_WHITE and on["mean"] - off["mean"] >= FLASH_MIN_GAIN
    print(f"flashbang: on mean {on['mean']:.1f} white {on['white']:.3f}, off mean {off['mean']:.1f}, "
          f"gain {on['mean'] - off['mean']:.1f} -> {'PASS' if flash_ok else 'FAIL'}")
    ok &= flash_ok and still_ok

    on, off = r["shk_conc_on"], r["shk_conc_off"]
    ratio = on["edge"] / max(off["edge"], 1e-6)
    shift = on["mean"] - off["mean"]
    conc_ok = (ratio <= BLUR_MAX_RATIO or abs(shift) >= CONC_MIN_SHIFT) and still_ok
    print(f"concussion: edge on {on['edge']:.2f} off {off['edge']:.2f} ratio {ratio:.3f}, mean shift {shift:+.1f} "
          f"(needs ratio <= {BLUR_MAX_RATIO} or |shift| >= {CONC_MIN_SHIFT:.0f}) -> {'PASS' if conc_ok else 'FAIL'}")
    sharp_ratio = on["sharp"] / max(off["sharp"], 1e-6)
    print(f"concussion (info): fine/coarse on {on['sharp']:.3f} off {off['sharp']:.3f} ratio {sharp_ratio:.3f}, "
          f"(well under 1 means fine detail was smeared, not just a brightness change)")
    ok &= conc_ok
    print("RESULT " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write-cfg", help="write the screenshot config")
    w.add_argument("path", type=Path)
    c = sub.add_parser("check", help="measure the seven screenshots")
    c.add_argument("dir", type=Path)
    args = parser.parse_args()
    if args.cmd == "write-cfg":
        args.path.write_text(CFG_TEXT)
        print(f"wrote {args.path}")
        return 0
    return check(args.dir)


if __name__ == "__main__":
    sys.exit(main())
