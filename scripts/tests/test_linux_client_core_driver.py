#!/usr/bin/env python3
"""Reject incomplete or contradictory client runtime-test evidence."""
import contextlib
import io
from types import SimpleNamespace
import unittest
import test_linux_client_core as checker

class RuntimeEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.old_failures, self.old_checks = checker.failures, checker.checks
        checker.failures, checker.checks = [], 0

    def tearDown(self):
        checker.failures, checker.checks = self.old_failures, self.old_checks

    def verify(self, output, code=0):
        with contextlib.redirect_stdout(io.StringIO()):
            checker.check_runtime_output('state', SimpleNamespace(
                stdout=output, stderr='', returncode=code), ('parser', 'input'))
        return not checker.failures

    def test_complete_results(self):
        self.assertTrue(self.verify('parser: 20 checks, 0 failures\ninput: 10 checks, 0 failures\n-- 30 checks, 0 failures --\n'))

    def test_missing_section(self):
        self.assertFalse(self.verify('parser: 30 checks, 0 failures\n-- 30 checks, 0 failures --\n'))

    def test_empty_section(self):
        self.assertFalse(self.verify('parser: 30 checks, 0 failures\ninput: 0 checks, 0 failures\n-- 30 checks, 0 failures --\n'))

    def test_section_failure_cannot_hide_behind_total(self):
        self.assertFalse(self.verify('parser: 20 checks, 1 failures\ninput: 10 checks, 0 failures\n-- 30 checks, 0 failures --\n'))

    def test_total_failure(self):
        self.assertFalse(self.verify('parser: 20 checks, 0 failures\ninput: 10 checks, 0 failures\n-- 30 checks, 1 failures --\n'))

    def test_inconsistent_total(self):
        self.assertFalse(self.verify('parser: 20 checks, 0 failures\ninput: 10 checks, 0 failures\n-- 31 checks, 0 failures --\n'))

    def test_exit_status_is_required(self):
        self.assertFalse(self.verify('parser: 20 checks, 0 failures\ninput: 10 checks, 0 failures\n-- 30 checks, 0 failures --\n', 1))

    def test_no_summary(self):
        self.assertFalse(self.verify('parser: 20 checks, 0 failures\ninput: 10 checks, 0 failures\n'))

if __name__ == '__main__':
    unittest.main()
