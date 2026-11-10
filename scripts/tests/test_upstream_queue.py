#!/usr/bin/env python3
"""Self-test of scripts/tools/upstream_queue.py and a rule check of the real queue.

Needs only git and Python. It never contacts upstream: the apply checks run
against a throwaway repository made here.
"""

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('upstream_queue', ROOT / 'scripts/tools/upstream_queue.py')
upstream_queue = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(upstream_queue)

PATCH = """From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001
From: Someone <someone@example.com>
Date: Tue, 10 Nov 2026 04:15:32 +0000
Subject: [PATCH] %(subject)s

%(body)s
---
 %(path)s | 2 +-
 1 file changed, 1 insertion(+), 1 deletion(-)

diff --git a/%(path)s b/%(path)s
index 1111111..2222222 100644
--- a/%(path)s
+++ b/%(path)s
@@ -1,3 +1,3 @@
 one
-two
+%(new)s
 three
"""


def patch(subject='fix: count to three', body='The second line was wrong.', path='src/a.cpp', new='2'):
    return PATCH % {'subject': subject, 'body': body, 'path': path, 'new': new}


def git(directory, *args):
    subprocess.run(['git', '-C', str(directory)] + list(args), check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


class LintTests(unittest.TestCase):
    def problems(self, **changes):
        return upstream_queue.lint_patch(changes.pop('name', '0001-count.patch'), patch(**changes))

    def test_a_short_plain_patch_passes(self):
        self.assertEqual(self.problems(), [])

    def test_scoped_subjects_pass(self):
        self.assertEqual(self.problems(subject='refactor(database): name DB zone flags'), [])

    def test_subject_must_use_upstreams_style(self):
        self.assertTrue(self.problems(subject='Count to three'))
        self.assertTrue(self.problems(subject='fix: ' + 'x' * 90))

    def test_hash_numbers_are_refused_in_subject_and_body(self):
        self.assertTrue(self.problems(subject='fix: count to three (#12)'))
        self.assertTrue(self.problems(body='Closes #52.'))
        self.assertEqual(self.problems(body='Answers upstream issue 52.'), [])

    def test_fork_notes_and_session_trailers_are_refused(self):
        for body in ('Claude-Session: https://claude.ai/code/session_x',
                     'See koalabuttz/KisakCOD.',
                     'Seen in fork PR 62.',
                     'Crashed on the laptop.'):
            with self.subTest(body=body):
                self.assertTrue(self.problems(body=body))

    def test_long_or_empty_bodies_are_refused(self):
        self.assertTrue(self.problems(body='\n'.join('line %d' % i for i in range(13))))
        self.assertTrue(self.problems(body=''))
        self.assertEqual(self.problems(body='\n'.join('line %d' % i for i in range(12))), [])

    def test_fork_only_paths_are_refused(self):
        for path in ('src/linux/compat.h', 'scripts/tests/test_x.py', 'scripts/tools/x.py', 'docs/status.md'):
            with self.subTest(path=path):
                self.assertTrue(self.problems(path=path))

    def test_file_names_are_numbered(self):
        self.assertTrue(self.problems(name='count.patch'))


class ApplyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.temp.name)
        self.upstream = root / 'upstream'
        (self.upstream / 'src').mkdir(parents=True)
        (self.upstream / 'src/a.cpp').write_text('one\ntwo\nthree\n')
        (self.upstream / 'src/b.cpp').write_text('one\ntwo\nthree\n')
        git(self.upstream, 'init', '-q')
        git(self.upstream, '-c', 'user.name=t', '-c', 'user.email=t@example.com', 'add', '-A')
        git(self.upstream, '-c', 'user.name=t', '-c', 'user.email=t@example.com', 'commit', '-qm', 'base')
        self.queue = root / 'queue'
        (self.queue / 'patches').mkdir(parents=True)
        base = subprocess.run(['git', '-C', str(self.upstream), 'rev-parse', 'HEAD'], check=True,
                              stdout=subprocess.PIPE, text=True).stdout
        (self.queue / 'BASE').write_text(base)

    def tearDown(self):
        self.temp.cleanup()

    def add(self, name, **changes):
        (self.queue / 'patches' / name).write_text(patch(**changes))

    def run_tool(self, command):
        return upstream_queue.main([command, '--queue', str(self.queue), '--upstream', str(self.upstream)])

    def test_independent_patches_pass_and_leave_the_tree_clean(self):
        self.add('0001-a.patch')
        self.add('0002-b.patch', path='src/b.cpp')
        self.assertEqual(self.run_tool('check'), 0)
        self.assertEqual((self.upstream / 'src/a.cpp').read_text(), 'one\ntwo\nthree\n')

    def test_apply_leaves_the_queue_applied(self):
        self.add('0001-a.patch')
        self.assertEqual(self.run_tool('apply'), 0)
        self.assertEqual((self.upstream / 'src/a.cpp').read_text(), 'one\n2\nthree\n')

    def test_two_patches_changing_the_same_line_fail(self):
        self.add('0001-a.patch')
        self.add('0002-a-again.patch', new='zwei')
        self.assertEqual(self.run_tool('check'), 1)

    def test_a_patch_that_does_not_apply_fails(self):
        (self.upstream / 'src/a.cpp').write_text('1\n2\n3\n')
        git(self.upstream, '-c', 'user.name=t', '-c', 'user.email=t@example.com', 'commit', '-qam', 'moved')
        base = subprocess.run(['git', '-C', str(self.upstream), 'rev-parse', 'HEAD'], check=True,
                              stdout=subprocess.PIPE, text=True).stdout
        (self.queue / 'BASE').write_text(base)
        self.add('0001-a.patch')
        self.assertEqual(self.run_tool('check'), 1)

    def test_a_tree_off_the_pinned_base_fails(self):
        self.add('0001-a.patch')
        (self.queue / 'BASE').write_text('0' * 40 + '\n')
        self.assertEqual(self.run_tool('check'), 1)

    def test_an_empty_queue_fails(self):
        self.assertEqual(self.run_tool('check'), 1)


class RealQueueTests(unittest.TestCase):
    def test_the_real_queue_keeps_the_rules(self):
        self.assertEqual(upstream_queue.main(['lint']), 0)


if __name__ == '__main__':
    unittest.main()
