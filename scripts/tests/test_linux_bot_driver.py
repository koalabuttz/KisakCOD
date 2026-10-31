#!/usr/bin/env python3
"""Asset-free checks that the live bot probe cannot mistake missing bots for a pass."""
import unittest
from test_linux_bots import (FIXTURE_GSC, transition_fixture, transition_plan,
                             validate_hold, validate_transitions)


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


def healthy_transitions(cycles=1):
    result = ''
    for stage, (_, map_name) in enumerate(transition_plan('mp_crash', cycles)):
        segment = healthy_log()
        if stage:
            segment = segment.replace('spawned entity=', 'retained entity=')
        result += (f'0:00 BOTFEATURE stage={stage} map={map_name}\n'
                   '0:00 BOTFEATURE auth_loopback=0 auth_unverified=0\n' +
                   ''.join(f'0:01 BOTFEATURE identity entity={i} name=bot{i}\n' for i in range(3)) +
                   segment + '0:08 BOTFEATURE hold=done\n')
    return result


class TransitionEvidenceTests(unittest.TestCase):
    def passed(self, log, cycles=1):
        return all(ok for ok, _ in validate_transitions(log, 3, 2, 'mp_crash', cycles))

    def test_complete_repeated_cycles(self):
        self.assertTrue(self.passed(healthy_transitions()))
        self.assertTrue(self.passed(healthy_transitions(2), 2))

    def test_every_evidence_line_required_once(self):
        log = healthy_transitions()
        self.assertFalse(self.passed(''))
        for index, line in enumerate(log.splitlines(keepends=True)):
            with self.subTest(line=index):
                self.assertFalse(self.passed(log.replace(line, '', 1)))
                self.assertFalse(self.passed(log.replace(line, line + line, 1)))

    def test_identity_and_lifecycle_mutations(self):
        log = healthy_transitions()
        for old, new in [('stage=1', 'stage=9'), ('map=mp_shipment', 'map=mp_crash'),
                         ('retained entity=2', 'retained entity=7'),
                         ('retained entity=2', 'retained entity=1'),
                         ('retained entity=2', 'spawned entity=2'),
                         ('identity entity=2 name=bot2', 'identity entity=2 name=new_bot'),
                         ('auth_loopback=0', 'auth_loopback=1'),
                         ('auth_unverified=0', 'auth_unverified=1'),
                         ('health=100', 'health=0'), ('sample=1', 'sample=9')]:
            with self.subTest(new=new):
                self.assertFalse(self.passed(log.replace(old, new, 1)))

    def test_hold_ids_must_match_identity_ids(self):
        log = healthy_transitions()
        # Admission and hold agree internally, but not with identity evidence.
        log = log.replace('spawned entity=2', 'spawned entity=7').replace('held entity=2', 'held entity=7')
        self.assertFalse(self.passed(log))

    def test_missing_extra_or_reordered_stage(self):
        log = healthy_transitions()
        stages = log.split('0:00 BOTFEATURE stage=')
        self.assertFalse(self.passed('0:00 BOTFEATURE stage='.join(stages[:-1])))
        self.assertFalse(self.passed(log + '0:00 BOTFEATURE stage=' + stages[-1]))
        stages[2], stages[3] = stages[3], stages[2]
        self.assertFalse(self.passed('0:00 BOTFEATURE stage='.join(stages)))

    def test_fixture_admits_only_initial_stage_and_restores_exact_identity(self):
        fixture = transition_fixture()
        self.assertEqual(fixture.count('addtestclient()'), 1)
        self.assertLess(fixture.index('if (stage == 0)'), fixture.index('addtestclient()'))
        callback = fixture.split('botTest_RestoreConnecting()')[1]
        self.assertIn('self getentitynumber() == getdvarint("botTest_entity_" + i)', callback)
        self.assertIn('self.name == getdvar("botTest_name_" + i)', callback)
        self.assertLess(callback.index('self.pers["isBot"] = true'),
                        callback.index('[[level.botTestOriginalConnect]]();'))
        self.assertIn('level.callbackPlayerConnect = ::botTest_RestoreConnecting;', fixture)
        self.assertNotIn('botTest_stage', FIXTURE_GSC)
        self.assertEqual(transition_plan('mp_crash', 0), [(None, 'mp_crash')])


if __name__ == '__main__':
    unittest.main()
