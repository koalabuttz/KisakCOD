#!/usr/bin/env python3
"""Prepare private UI PCM resources from an actual offline DB sound export.

No extraction, downloads, format guessing, existing-file replacement, or assets
in source control. Gain policy: exported alias volume, unity master/channel.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct

ALIASES = ("mouse_click", "mouse_over")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def prepare(inventory, output):
    inventory, output = Path(inventory), Path(output)
    require(output.is_dir(), "Output must be an existing private bundle directory")
    require(inventory.stat().st_size <= 65536, "Sound inventory exceeds bound")
    document = json.loads(inventory.read_text())
    require(isinstance(document, dict), "Invalid sound inventory object")
    entries = document.get("aliases")
    require(isinstance(entries, list) and len(entries) == 2, "Expected exactly two exported aliases")
    require(all(isinstance(entry, dict) for entry in entries), "Invalid alias record")
    by_name = {entry.get("name"): entry for entry in entries}
    require(set(by_name) == set(ALIASES), "Missing or duplicate UI alias")
    prepared = []
    for name in ALIASES:
        entry = by_name[name]
        require(entry.get("available") is True and type(entry.get("count")) is int and entry["count"] == 1,
                "Alias must have exactly one available variant")
        variants = entry.get("variants")
        require(isinstance(variants, list) and len(variants) == 1, "Alias variant count mismatch")
        variant = variants[0]
        require(isinstance(variant, dict), "Invalid alias variant")
        require(variant.get("name") == name and type(variant.get("flags")) is int and variant["flags"] == 2648,
                "Unsupported alias name or flags")
        require("secondary" in variant and "chain" in variant and variant["secondary"] is None and variant["chain"] is None
                and type(variant.get("start_delay")) is int and variant["start_delay"] == 0,
                "Secondary, chain, and delay are unsupported")
        volume, pitch = variant.get("volume"), variant.get("pitch")
        require(isinstance(volume, list) and len(volume) == 2 and all(number(v) for v in volume)
                and volume[0] == volume[1] and 0 <= volume[0] <= 1, "Unsupported alias volume range")
        require(isinstance(pitch, list) and len(pitch) == 2 and all(number(v) and v == 1 for v in pitch),
                "Only fixed pitch1 is supported")
        sound = variant.get("sound_file")
        require(isinstance(sound, dict), "Missing loaded sound metadata")
        for field, expected in {"type": 1, "exists": 1, "format": 1, "rate": 44100, "bits": 16, "channels": 1, "block_size": 2}.items():
            require(type(sound.get(field)) is int and sound[field] == expected, "Unsupported sound " + field)
        require(sound.get("data_present") is True, "Loaded PCM is absent")
        samples, length = sound.get("samples"), sound.get("data_len")
        require(type(samples) is int and 0 < samples <= 88200 and type(length) is int and length == samples * 2,
                "Invalid PCM sample count/length")
        filename = sound.get("pcm_file")
        require(isinstance(filename, str) and filename not in ("", ".", "..") and "/" not in filename and "\\" not in filename,
                "PCM must be a local export filename")
        pcm_path = inventory.parent / filename
        require(not pcm_path.is_symlink() and pcm_path.is_file() and pcm_path.stat().st_size == length,
                "PCM file size/type mismatch")
        pcm = pcm_path.read_bytes()
        require(len(pcm) == length, "PCM changed during preparation")
        header = b"KUASND01" + struct.pack("<6I2f3I", 2648, 1, 44100, 16, 1, 2, 1, volume[0], samples, length, 0)
        prepared.append((output / (name + ".sound"), header + pcm))
    require(not any(os.path.lexists(path) for path, _ in prepared), "Refusing to overwrite an existing sound resource")
    created = []
    try:
        for path, data in prepared:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            created.append(path)
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
    except BaseException:
        for path in created:
            path.unlink()
        raise
    return {path.name: hashlib.sha256(data).hexdigest() for path, data in prepared}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.inventory, args.output_dir), indent=2))


if __name__ == "__main__":
    main()
