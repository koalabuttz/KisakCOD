#!/usr/bin/env python3
"""Compares two frames of the same view, image by image.

The world draw probe writes each view's settled frame as a binary PPM with
KISAK_WORLD_DRAW_IMAGES=1 (view-A.ppm, view-B.ppm beside its report). Running it
once with r_backend d3d9 (DXVK) and once with r_backend vulkan gives two images
of the same view, and this script says how far apart they are:

  mean_abs        mean absolute difference per channel, 0..255
  max_abs         the largest difference of any channel of any pixel
  over_8, over_32 share of pixels whose largest channel difference is above 8
                  (rounding, filtering) or above 32 (something drawn differently)
  psnr_db         peak signal-to-noise ratio over all channels (inf when equal)

With --diff it writes the per-pixel difference, scaled by --gain, as a PPM, so
the places that differ show up bright. Reads and writes PPM only; standard
library only. Nothing is compared against a stored reference: both images come
from the machine the probe ran on, and stay there.

Exit status: 0 when the images were compared (and every --max-* limit held),
1 when a limit was exceeded, 2 on a usage error or an unreadable image.

Usage:
  compare_frames.py <reference.ppm> <candidate.ppm> [--diff out.ppm] [--gain 4]
      [--max-mean-abs X] [--max-over-32 X] [--min-psnr X]
"""

import argparse
import math
import sys


class ImageError(Exception):
    pass


def _token(data, pos):
    """The next whitespace-separated header token, skipping # comments."""
    while pos < len(data):
        c = data[pos:pos + 1]
        if c == b'#':
            while pos < len(data) and data[pos:pos + 1] not in (b'\n', b'\r'):
                pos += 1
        elif c.isspace():
            pos += 1
        else:
            break
    start = pos
    while pos < len(data) and not data[pos:pos + 1].isspace() and data[pos:pos + 1] != b'#':
        pos += 1
    if start == pos:
        raise ImageError('truncated header')
    return data[start:pos], pos


def read_ppm(path):
    """(width, height, rgb bytes) of a binary PPM (P6) with maxval 255."""
    try:
        with open(path, 'rb') as handle:
            data = handle.read()
    except OSError as error:
        raise ImageError(f'{path}: {error.strerror}') from error
    try:
        magic, pos = _token(data, 0)
        width, pos = _token(data, pos)
        height, pos = _token(data, pos)
        maxval, pos = _token(data, pos)
        if magic != b'P6':
            raise ImageError('not a binary PPM (P6)')
        width, height, maxval = int(width), int(height), int(maxval)
    except ValueError as error:
        raise ImageError(f'{path}: bad header') from error
    except ImageError as error:
        raise ImageError(f'{path}: {error}') from error
    if width <= 0 or height <= 0 or maxval != 255:
        raise ImageError(f'{path}: needs a positive size and maxval 255')
    pixels = data[pos + 1:]  # one whitespace byte ends the header
    if len(pixels) != width * height * 3:
        raise ImageError(f'{path}: {len(pixels)} pixel bytes, expected {width * height * 3}')
    return width, height, pixels


def write_ppm(path, width, height, rgb):
    with open(path, 'wb') as handle:
        handle.write(b'P6\n%d %d\n255\n' % (width, height))
        handle.write(rgb)


def compare(reference, candidate, gain=4):
    """The measures above, and the scaled difference image."""
    if len(reference) != len(candidate):
        raise ImageError('the images differ in size')
    pixels = len(reference) // 3
    total = 0
    squares = 0
    max_abs = 0
    over_8 = 0
    over_32 = 0
    diff = bytearray(len(reference))
    for i in range(pixels):
        worst = 0
        for c in range(3):
            d = abs(reference[3 * i + c] - candidate[3 * i + c])
            total += d
            squares += d * d
            worst = max(worst, d)
            diff[3 * i + c] = min(255, d * gain)
        max_abs = max(max_abs, worst)
        over_8 += worst > 8
        over_32 += worst > 32
    samples = pixels * 3
    mse = squares / samples
    return {
        'mean_abs': total / samples,
        'max_abs': max_abs,
        'over_8': over_8 / pixels,
        'over_32': over_32 / pixels,
        'psnr_db': math.inf if mse == 0 else 10 * math.log10(255 * 255 / mse),
    }, bytes(diff)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reference')
    parser.add_argument('candidate')
    parser.add_argument('--diff', help='write the scaled difference here (PPM)')
    parser.add_argument('--gain', type=int, default=4, help='difference scale for --diff (default 4)')
    parser.add_argument('--max-mean-abs', type=float)
    parser.add_argument('--max-over-32', type=float, help='share of pixels, 0..1')
    parser.add_argument('--min-psnr', type=float)
    args = parser.parse_args(argv)
    if args.gain < 1:
        parser.error('--gain must be at least 1')

    try:
        w1, h1, a = read_ppm(args.reference)
        w2, h2, b = read_ppm(args.candidate)
        if (w1, h1) != (w2, h2):
            raise ImageError(f'the images differ in size: {w1}x{h1} and {w2}x{h2}')
        measures, diff = compare(a, b, args.gain)
    except ImageError as error:
        print(f'compare_frames: {error}', file=sys.stderr)
        return 2
    if args.diff:
        write_ppm(args.diff, w1, h1, diff)

    psnr = 'inf' if math.isinf(measures['psnr_db']) else f"{measures['psnr_db']:.2f}"
    print(f"compare size={w1}x{h1} mean_abs={measures['mean_abs']:.3f} max_abs={measures['max_abs']} "
          f"over_8={measures['over_8']:.4f} over_32={measures['over_32']:.4f} psnr_db={psnr}")
    failed = []
    if args.max_mean_abs is not None and measures['mean_abs'] > args.max_mean_abs:
        failed.append(f'mean_abs above {args.max_mean_abs}')
    if args.max_over_32 is not None and measures['over_32'] > args.max_over_32:
        failed.append(f'over_32 above {args.max_over_32}')
    if args.min_psnr is not None and measures['psnr_db'] < args.min_psnr:
        failed.append(f'psnr_db below {args.min_psnr}')
    print('compare=' + ('failed ' + '; '.join(failed) if failed else 'done'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
