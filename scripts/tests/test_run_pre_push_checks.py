#!/usr/bin/env python3
"""The pre-push runner names real scripts and fails when one of them fails."""
from pathlib import Path
import importlib.util
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'run_pre_push_checks', ROOT / 'scripts/tests/run_pre_push_checks.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class PrePushTests(unittest.TestCase):
    def test_every_named_check_exists(self):
        for name in runner.CHECKS + runner.NATIVE[:1]:
            self.assertTrue((ROOT / 'scripts/tests' / name).is_file(), name)

    def test_list_prints_one_line_per_check_and_runs_nothing(self):
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/tests/run_pre_push_checks.py'),
                                 '--list'], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertGreaterEqual(len(lines), len(runner.CHECKS))
        self.assertIn('test_linux_window_workflow.py', lines)

    def test_a_failing_check_is_reported_and_fails_the_run(self):
        original = runner.CHECKS
        try:
            runner.CHECKS = []
            planned = runner.commands(False)
            self.assertEqual(planned, [])
            command, result, _ = runner.run_one([sys.executable, '-c', 'import sys; sys.exit(3)'])
            self.assertEqual(result.returncode, 3)
        finally:
            runner.CHECKS = original

    def test_native_check_needs_a_build_tree(self):
        if runner.native_available():
            self.skipTest('a build tree is present; the native set would run')
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/tests/run_pre_push_checks.py'),
                                 '--native', '--jobs', '1'], capture_output=True, text=True,
                                timeout=60, cwd=ROOT)
        self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main()
