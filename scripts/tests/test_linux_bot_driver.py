#!/usr/bin/env python3
"""Asset-free checks that the live bot probe cannot mistake missing bots for a pass."""
import unittest
from test_linux_bots import validate_hold


def healthy_log():
    log = ''.join(f'0:01 BOTFEATURE spawned entity={i}\n' for i in range(3))
    for sample in range(3):
        for i in range(3):
            log += (f'0:05 BOTFEATURE held entity={i} sample={sample} state=playing '
                    'health=100 team=allies class=CLASS_ASSAULT weapon=m16_mp\n')
    return log


class HoldEvidenceTests(unittest.TestCase):
    def passed(self, log):
        return all(ok for ok, _ in validate_hold(log, 3, 2))

    def test_complete_samples(self):
        self.assertTrue(self.passed(healthy_log()))

    def test_empty_or_missing(self):
        self.assertFalse(self.passed(''))
        for line in healthy_log().splitlines(keepends=True):
            self.assertFalse(self.passed(healthy_log().replace(line, '', 1)))

    def test_duplicate_cannot_replace_missing_client(self):
        self.assertFalse(self.passed(healthy_log().replace('held entity=2 sample=1',
                                                         'held entity=1 sample=1')))

    def test_duplicate_samples_and_admissions(self):
        for line in healthy_log().splitlines(keepends=True):
            self.assertFalse(self.passed(healthy_log() + line))

    def test_health_team_class_weapon_and_sample_are_checked(self):
        for old, new in [('health=100', 'health=0'), ('health=100', 'health=-5'),
                         ('state=playing', 'state=spectator'), ('team=allies', 'team=axis'),
                         ('class=CLASS_ASSAULT', 'class=undefined'),
                         ('weapon=m16_mp', 'weapon=none'), ('weapon=m16_mp', 'weapon='),
                         ('sample=1', 'sample=9')]:
            with self.subTest(new=new):
                self.assertFalse(self.passed(healthy_log().replace(old, new, 1)))


if __name__ == '__main__':
    unittest.main()
