#!/usr/bin/env python3
"""Say how loud each speaker is in a recorded mix.

Not a CI check: it reads what the game wrote through OpenAL Soft's wave writer
(``ALSOFT_DRIVERS=wave``, docs/linux-mp-client.md "Speakers"), so a sound run on
a test machine can say which speakers the game used without anyone listening.
Standard library only.

    wav_levels.py run.wav [more.wav ...]            per-speaker levels of each file
    wav_levels.py --window 5 run.wav                 also per 5 s stretch
    wav_levels.py --json levels.json a.wav b.wav     the numbers as JSON too

Reads RIFF WAVE in 16, 24 or 32-bit integers or 32-bit float, plain or
WAVE_FORMAT_EXTENSIBLE (what the wave writer writes for more than two channels).
For each speaker: RMS and peak in dBFS, its share of the file's total power, and
the share of 50 ms blocks louder than -60 dBFS (how often the speaker carried
anything). Six channels are named in the order OpenAL Soft writes 5.1:
L R C LFE SL SR.
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

NAMES = {
    1: ["mono"],
    2: ["L", "R"],
    4: ["FL", "FR", "BL", "BR"],
    6: ["L", "R", "C", "LFE", "SL", "SR"],
    8: ["L", "R", "C", "LFE", "BL", "BR", "SL", "SR"],
}
SILENT_DB = -60.0
BLOCK_SECONDS = 0.05
FLOAT_GUID_TAIL = b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"


class WavError(Exception):
    pass


def read_wav(path):
    """Returns (rate, channels, frames as a list of per-channel lists of floats in -1..1)."""
    data = Path(path).read_bytes()
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise WavError(f"{path}: not a RIFF WAVE file")
    at, fmt, body = 12, None, None
    while at + 8 <= len(data):
        kind, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        chunk = data[at + 8:at + 8 + size]
        if kind == b"fmt ":
            fmt = chunk
        elif kind == b"data":
            body = chunk
            # The wave writer fills the sizes in when it closes; a game that was killed
            # leaves 0 or a short count, so take everything that is there.
            if size == 0 or size > len(data) - at - 8:
                body = data[at + 8:]
        at += 8 + size + (size & 1)
        if body is not None and fmt is not None:
            break
    if fmt is None or body is None:
        raise WavError(f"{path}: no fmt or data chunk")
    tag, channels, rate = struct.unpack_from("<HHI", fmt, 0)
    bits = struct.unpack_from("<H", fmt, 14)[0]
    if tag == 0xFFFE and len(fmt) >= 40:
        sub = fmt[24:40]
        tag = struct.unpack_from("<H", sub, 0)[0] if sub[2:] == FLOAT_GUID_TAIL else -1
    if channels < 1:
        raise WavError(f"{path}: {channels} channels")
    width = bits // 8
    frame = width * channels
    count = len(body) // frame
    if tag == 3 and bits == 32:
        values = struct.unpack_from(f"<{count * channels}f", body, 0)
    elif tag == 1 and bits == 16:
        values = [v / 32768.0 for v in struct.unpack_from(f"<{count * channels}h", body, 0)]
    elif tag == 1 and bits == 32:
        values = [v / 2147483648.0 for v in struct.unpack_from(f"<{count * channels}i", body, 0)]
    elif tag == 1 and bits == 24:
        values = []
        for i in range(count * channels):
            b = body[i * 3:i * 3 + 3]
            values.append(int.from_bytes(b, "little", signed=True) / 8388608.0)
    else:
        raise WavError(f"{path}: format {tag} with {bits} bits is not read here")
    return rate, channels, [list(values[c::channels]) for c in range(channels)]


def db(value):
    return 20.0 * math.log10(value) if value > 0 else float("-inf")


def levels(samples, rate, start=0, end=None):
    end = len(samples) if end is None else end
    part = samples[start:end]
    if not part:
        return {"rms_db": float("-inf"), "peak_db": float("-inf"), "power": 0.0, "active": 0.0}
    power = sum(v * v for v in part) / len(part)
    block = max(1, int(rate * BLOCK_SECONDS))
    blocks = active = 0
    for at in range(0, len(part), block):
        piece = part[at:at + block]
        blocks += 1
        if db(math.sqrt(sum(v * v for v in piece) / len(piece))) > SILENT_DB:
            active += 1
    return {"rms_db": db(math.sqrt(power)), "peak_db": db(max(abs(v) for v in part)),
            "power": power, "active": active / blocks}


def describe(path, rate, channels, data, window):
    names = NAMES.get(channels, [f"ch{c}" for c in range(channels)])
    frames = len(data[0]) if data else 0
    whole = [levels(data[c], rate) for c in range(channels)]
    total = sum(l["power"] for l in whole) or 1.0
    result = {"file": str(path), "rate": rate, "channels": channels, "seconds": frames / rate if rate else 0,
              "speakers": {}, "windows": []}
    for c in range(channels):
        entry = dict(whole[c])
        entry["share"] = entry["power"] / total
        result["speakers"][names[c]] = entry
    if window and rate:
        step = int(window * rate)
        for start in range(0, frames, step):
            per = [levels(data[c], rate, start, start + step) for c in range(channels)]
            result["windows"].append({"start": start / rate,
                                      "rms_db": {names[c]: per[c]["rms_db"] for c in range(channels)}})
    return result


def fmt_db(value):
    return "  -inf" if value == float("-inf") else f"{value:6.1f}"


def print_result(result):
    print(f"{result['file']}: {result['channels']} channels, {result['rate']} Hz, {result['seconds']:.1f} s")
    print("  speaker   RMS dBFS  peak dBFS  power share  active")
    for name, entry in result["speakers"].items():
        print(f"  {name:<8} {fmt_db(entry['rms_db'])}     {fmt_db(entry['peak_db'])}     {entry['share'] * 100:6.1f} %"
              f"   {entry['active'] * 100:5.1f} %")
    if result["windows"]:
        names = list(result["speakers"])
        print("  window    " + " ".join(f"{n:>6}" for n in names))
        for window in result["windows"]:
            print(f"  {window['start']:6.1f} s " + " ".join(fmt_db(window["rms_db"][n]) for n in names))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--window", type=float, default=0.0, help="also give levels per stretch of this many seconds")
    parser.add_argument("--json", type=Path, help="write the numbers here too")
    args = parser.parse_args(argv)
    results = []
    for path in args.files:
        try:
            rate, channels, data = read_wav(path)
        except (OSError, WavError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        result = describe(path, rate, channels, data, args.window)
        print_result(result)
        results.append(result)
    if args.json:
        def clean(value):
            if isinstance(value, float) and math.isinf(value):
                return None
            if isinstance(value, dict):
                return {k: clean(v) for k, v in value.items()}
            if isinstance(value, list):
                return [clean(v) for v in value]
            return value
        args.json.write_text(json.dumps(clean(results), indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
