#!/usr/bin/env python3
"""Checks scripts/tools/vulkan/compare_frames.py on small generated images."""

import importlib.util
import io
import math
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('compare_frames', ROOT / 'scripts' / 'tools' / 'vulkan' / 'compare_frames.py')
compare_frames = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare_frames)


class CompareFramesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def image(self, name, width, height, rgb, header=None):
        path = self.dir / name
        with open(path, 'wb') as handle:
            handle.write(header if header is not None else b'P6\n%d %d\n255\n' % (width, height))
            handle.write(bytes(rgb))
        return str(path)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            status = compare_frames.main(list(args))
        return status, out.getvalue(), err.getvalue()

    def test_equal_images(self):
        a = self.image('a.ppm', 2, 1, [10, 20, 30, 40, 50, 60])
        status, out, _ = self.run_main(a, a)
        self.assertEqual(status, 0)
        self.assertIn('mean_abs=0.000 max_abs=0 over_8=0.0000 over_32=0.0000 psnr_db=inf', out)

    def test_measures(self):
        a = self.image('a.ppm', 2, 1, [0, 0, 0, 100, 100, 100])
        b = self.image('b.ppm', 2, 1, [0, 0, 6, 100, 140, 100])
        diff = str(self.dir / 'diff.ppm')
        status, out, _ = self.run_main(a, b, '--diff', diff, '--gain', '2')
        self.assertEqual(status, 0)
        self.assertIn('mean_abs=7.667 max_abs=40 over_8=0.5000 over_32=0.5000', out)
        mse = (36 + 1600) / 6
        self.assertIn(f'psnr_db={10 * math.log10(255 * 255 / mse):.2f}', out)
        self.assertEqual(compare_frames.read_ppm(diff), (2, 1, bytes([0, 0, 12, 0, 80, 0])))

    def test_limits(self):
        a = self.image('a.ppm', 1, 1, [0, 0, 0])
        b = self.image('b.ppm', 1, 1, [0, 0, 64])
        status, out, _ = self.run_main(a, b, '--max-over-32', '0.5', '--min-psnr', '30')
        self.assertEqual(status, 1)
        self.assertIn('compare=failed over_32 above 0.5; psnr_db below 30.0', out)

    def test_comments_in_the_header(self):
        a = self.image('a.ppm', 1, 1, [1, 2, 3], header=b'P6 # made here\n1 1\n# max\n255\n')
        self.assertEqual(compare_frames.read_ppm(a), (1, 1, bytes([1, 2, 3])))

    def test_refusals(self):
        a = self.image('a.ppm', 1, 1, [0, 0, 0])
        b = self.image('b.ppm', 2, 1, [0] * 6)
        short = self.image('short.ppm', 2, 2, [0] * 5)
        ascii_ppm = self.image('p3.ppm', 1, 1, b'0 0 0', header=b'P3\n1 1\n255\n')
        for args in ((a, b), (a, short), (a, ascii_ppm), (a, os.path.join(self.temp.name, 'missing.ppm'))):
            with self.subTest(args=args):
                status, out, err = self.run_main(*args)
                self.assertEqual(status, 2)
                self.assertEqual(out, '')
                self.assertIn('compare_frames:', err)


if __name__ == '__main__':
    unittest.main()
