#!/usr/bin/env python3
"""Asset-independent metadata preparation regression; no proprietary PCM."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("prepare_audio", Path(__file__).parents[1] / "tools/prepare_linux_ui_audio.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def inventory():
    entries = []
    for name in MODULE.ALIASES:
        entries.append(dict(name=name, available=True, count=1, variants=[dict(
            name=name, flags=2648, volume=[.25,.25], pitch=[1,1], start_delay=0, secondary=None, chain=None,
            sound_file=dict(type=1, exists=1, format=1, rate=44100, bits=16, channels=1, samples=2, data_len=4,
                            block_size=2, data_present=True, pcm_file=name+".pcm"))]))
    return dict(aliases=entries)


class PrepareTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root/"bundle"
        self.out.mkdir(mode=0o700)
        self.path = self.root/"ui-sounds.json"
        for name in MODULE.ALIASES:
            (self.root/(name+".pcm")).write_bytes(bytes([255,127,0,128]))

    def prepare(self, document):
        self.path.write_text(json.dumps(document))
        return MODULE.prepare(self.path, self.out)

    def test_valid_owned_bytes_and_no_overwrite(self):
        results = self.prepare(inventory())
        self.assertEqual(set(results), {"mouse_click.sound", "mouse_over.sound"})
        before = (self.out/"mouse_click.sound").read_bytes()
        self.assertEqual(before[:8], b"KUASND01")
        self.assertEqual(before[52:], bytes([255,127,0,128]))
        self.assertEqual((self.out/"mouse_click.sound").stat().st_mode & 0o777, 0o600)
        with self.assertRaises(ValueError):
            self.prepare(inventory())
        self.assertEqual((self.out/"mouse_click.sound").read_bytes(), before)

    def test_unsupported_alias_metadata_no_publication(self):
        changes = {"flags":2649, "volume":[.1,.2], "pitch":[1,2], "secondary":"other", "chain":"other", "start_delay":1}
        for field, value in changes.items():
            with self.subTest(field=field):
                doc=inventory();doc["aliases"][1]["variants"][0][field]=value
                with self.assertRaises(ValueError): self.prepare(doc)
                self.assertEqual(list(self.out.iterdir()), [])

    def test_unsupported_pcm_metadata(self):
        changes = {"type":2,"exists":0,"format":2,"rate":48000,"bits":8,"channels":2,"block_size":4,
                   "samples":88201,"data_len":3,"data_present":False,"pcm_file":"../outside.pcm"}
        for field, value in changes.items():
            with self.subTest(field=field):
                doc=inventory();doc["aliases"][0]["variants"][0]["sound_file"][field]=value
                with self.assertRaises(ValueError): self.prepare(doc)
                self.assertEqual(list(self.out.iterdir()), [])

    def test_variant_count_and_missing_chain_metadata(self):
        for kind in ["count", "variants", "missing", "duplicate", "nan", "bool"]:
            doc=inventory();entry=doc["aliases"][0];variant=entry["variants"][0]
            if kind=="count": entry["count"]=2
            elif kind=="variants": entry["variants"].append(copy.deepcopy(variant))
            elif kind=="missing": del variant["chain"]
            elif kind=="duplicate": doc["aliases"][1]=copy.deepcopy(entry)
            elif kind=="nan": variant["volume"]=[float("nan")]*2
            else: variant["pitch"]=[True,True]
            with self.subTest(kind=kind), self.assertRaises(ValueError): self.prepare(doc)
            self.assertEqual(list(self.out.iterdir()), [])

    def test_pcm_length_and_symlink(self):
        pcm=self.root/"mouse_click.pcm";pcm.write_bytes(b"\x00")
        with self.assertRaises(ValueError): self.prepare(inventory())
        pcm.unlink();pcm.symlink_to(self.root/"mouse_over.pcm")
        with self.assertRaises(ValueError): self.prepare(inventory())
        self.assertEqual(list(self.out.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
