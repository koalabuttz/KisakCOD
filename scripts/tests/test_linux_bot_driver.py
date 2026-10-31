#!/usr/bin/env python3
"""Asset-free checks that the live bot probe cannot mistake missing bots for a pass."""
import unittest
from test_linux_bots import (FIXTURE_GSC, transition_fixture, transition_plan,
                             final_cohort_log, refill_fixture, validate_refills,
                             validate_kick_window, validate_hold, validate_transitions)


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


def healthy_refills(cycles=3):
    log = '0:00 BOTFEATURE stage=0 map=mp_crash\n'
    for cycle in range(cycles + 1):
        log += f'0:00 BOTFEATURE refill={cycle} capacity=3\n'
        if cycle:
            log += ''.join(f'0:00 BOTFEATURE kick entity={i} name=bot{3*(cycle-1)+i}\n' for i in range(3))
            log += '0:00 BOTFEATURE vacant players=0\n'
            log += ''.join(f'0:00 BOTFEATURE vacated entity={i} name=bot{3*(cycle-1)+i}\n' for i in range(3))
        log += '0:00 BOTFEATURE auth_loopback=0 auth_unverified=0\n'
        log += ''.join(f'0:01 BOTFEATURE identity entity={i} name=bot{3*cycle+i}\n' for i in range(3))
        holds = healthy_log()
        holds = '\n'.join(line.replace('team=allies', 'team=axis') if 'held entity=1 ' in line else line
                          for line in holds.splitlines()) + '\n'
        log += holds + '0:08 BOTFEATURE hold=done\n'
    return log


class RefillEvidenceTests(unittest.TestCase):
    def passed(self, log):
        return all(ok for ok, _ in validate_refills(log, 3, 2, 3))

    def test_repeated_slot_reuse_beyond_capacity(self):
        self.assertTrue(self.passed(healthy_refills()))
        final = final_cohort_log(healthy_refills())
        self.assertTrue(all(ok for ok, _ in validate_transitions(final, 3, 2, 'mp_crash', 0, True)))
        self.assertIn('name=bot11', final)
        self.assertNotIn('name=bot0\n', final)

    def test_map_transitions_retain_final_not_original_cohort(self):
        maps = healthy_transitions()
        maps = maps[maps.index('0:00 BOTFEATURE stage=1'):]
        for i in range(3):
            maps = maps.replace(f'name=bot{i}\n', f'name=bot{i+9}\n')
        maps = '\n'.join(line.replace('team=allies', 'team=axis') if 'held entity=1 ' in line else line
                         for line in maps.splitlines()) + '\n'
        log = healthy_refills() + maps
        self.assertTrue(self.passed(log))
        final = final_cohort_log(log)
        self.assertTrue(all(ok for ok, _ in validate_transitions(final, 3, 2, 'mp_crash', 1, True)))
        self.assertFalse(all(ok for ok, _ in validate_transitions(final.replace('name=bot9', 'name=bot0', 1),
                                                                 3, 2, 'mp_crash', 1, True)))

    def test_missing_duplicate_or_changed_evidence(self):
        log = healthy_refills()
        for line in log.splitlines(keepends=True)[1:]:
            with self.subTest(line=line.strip()):
                self.assertFalse(self.passed(log.replace(line, '', 1)))
                self.assertFalse(self.passed(log.replace(line, line + line, 1)))
        for old, new in [('capacity=3', 'capacity=8'), ('refill=1', 'refill=2'),
                         ('vacant players=0', 'vacant players=1'),
                         ('kick entity=0 name=bot0', 'kick entity=0 name=bot9'),
                         ('vacated entity=0 name=bot0', 'vacated entity=0 name=bot9'),
                         ('identity entity=0 name=bot3', 'identity entity=0 name=bot0'),
                         ('auth_unverified=0', 'auth_unverified=1')]:
            with self.subTest(new=new):
                self.assertFalse(self.passed(log.replace(old, new, 1)))

    def test_unused_slots_cannot_replace_reuse(self):
        log = healthy_refills()
        start = log.index('BOTFEATURE refill=1')
        stop = log.index('BOTFEATURE refill=2')
        middle = log[start:stop]
        for marker in ('identity entity=', 'spawned entity=', 'held entity='):
            middle = middle.replace(marker + '0', marker + '7')
        self.assertFalse(self.passed(log[:start] + middle + log[stop:]))

    def test_kick_window_requires_exact_engine_lines(self):
        output = 'unrelated output\n0:bot0 EXE_PLAYERKICKED\n1:bot1 EXE_PLAYERKICKED\n'
        expected = {('0', 'bot0'), ('1', 'bot1')}
        self.assertTrue(validate_kick_window(output, expected))
        self.assertTrue(validate_kick_window('ordinary output\n', set()))
        self.assertFalse(validate_kick_window(output, set()))
        self.assertFalse(validate_kick_window('', expected))
        self.assertFalse(validate_kick_window(output + '0:bot0 EXE_PLAYERKICKED\n', expected))
        self.assertFalse(validate_kick_window(output.replace('bot1', 'bot2'), expected))
        self.assertFalse(validate_kick_window(output + 'malformed EXE_PLAYERKICKED\n', expected))

    def test_fixture_uses_real_kick_and_default_grace(self):
        fixture = refill_fixture()
        self.assertIn('kick(oldIds[i]);', fixture)
        self.assertIn('wait (getdvarint("sv_zombietime") + 2);', fixture)
        self.assertIn('getdvarint("botTest_refill") < round', fixture)
        self.assertNotIn('setdvar("sv_zombietime"', fixture)
        self.assertEqual(fixture.count('addtestclient()'), 1)


if __name__ == '__main__':
    unittest.main()
