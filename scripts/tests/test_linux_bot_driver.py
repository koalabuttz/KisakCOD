#!/usr/bin/env python3
"""Asset-free checks that the live bot probe cannot mistake missing bots for a pass."""
import unittest
from test_linux_bots import (FIXTURE_GSC, transition_fixture, transition_plan,
                             final_cohort_log, refill_fixture, validate_refills,
                             validate_kick_window, validate_hold, validate_transitions,
                             validate_brain_view, validate_brain_walk, balanced_teams,
                             uses_both_teams, TEAM_CAPACITY, MAX_BOTS, validate_brain_notices,
                             validate_brain_fights, COVER_ROW, HUNT_ROW)


def healthy_log():
    log = ''.join(f'0:01 BOTFEATURE spawned entity={i}\n' for i in range(3))
    for sample in range(3):
        for i in range(3):
            log += (f'0:05 BOTFEATURE held entity={i} sample={sample} state=playing '
                    'health=100 team=allies class=CLASS_ASSAULT weapon=m16_mp\n')
    return log


def view_log(yaws, pitch=0.0, bots=3, states=None):
    log = ''
    for tick, yaw in enumerate(yaws):
        for i in range(bots):
            state = f' state={states[tick]}' if states else ''
            log += f'0:05 BOTFEATURE view entity={i} tick={tick}{state} angles=({pitch}, {yaw + i}, 0)\n'
    return log


class BrainViewTests(unittest.TestCase):
    # Views are four a second: a one-second hold has five.
    def passed(self, log, hold=1):
        return all(ok for ok, _ in validate_brain_view(log, 3, hold))

    def test_a_bot_that_looks_around_passes(self):
        self.assertTrue(self.passed(view_log([0, 40, 80, 80, 30])))

    def test_turning_through_the_back_takes_the_short_way(self):
        self.assertTrue(self.passed(view_log([170, -170, -130, -130, -170])))

    def test_a_bot_that_never_turns_fails(self):
        self.assertFalse(self.passed(view_log([30, 30, 30, 31, 30])))

    def test_a_snap_faster_than_the_motor_fails(self):
        # 120 and 70 degrees in a quarter second: past the 200 deg/s top speed.
        self.assertFalse(self.passed(view_log([0, 120, 120, 120, 120])))
        self.assertFalse(self.passed(view_log([0, 0, 0, -70, -70])))
        # A turn the short way round past 180 a second ago is still caught.
        self.assertFalse(self.passed(view_log([0, 90, 180, -90, -90])))

    def test_pitch_past_the_limit_fails(self):
        self.assertFalse(self.passed(view_log([0, 40, 80, 80, 30], pitch=-89.0)))

    def test_missing_or_extra_samples_fail(self):
        log = view_log([0, 40, 80, 80, 30])
        self.assertFalse(self.passed(log.replace(log.splitlines(keepends=True)[4], '', 1)))
        self.assertFalse(self.passed(log + log.splitlines(keepends=True)[0]))
        self.assertFalse(self.passed(''))

    def test_a_fighting_bot_may_flick_faster(self):
        # 100 degrees in a quarter second: past walking, within the aiming hand.
        log = view_log([0, 100, 100, 100, 60])
        self.assertFalse(self.passed(log))
        self.assertTrue(all(ok for ok, _ in validate_brain_view(log, 3, 1, combat=True)))
        # 180 in a quarter second is past even the aiming hand.
        self.assertFalse(all(ok for ok, _ in validate_brain_view(view_log([0, 90, -90, -90, -60]), 3, 1,
                                                                 combat=True)))

    def test_a_death_and_spawn_is_no_turn(self):
        states = ['playing', 'dead', 'dead', 'playing', 'playing']
        self.assertTrue(self.passed(view_log([0, 40, 170, -60, -20], states=states)))
        # The same jump while playing is a snap.
        self.assertFalse(self.passed(view_log([0, 40, 170, -60, -20], states=['playing'] * 5)))

    def test_fixture_logs_the_view(self):
        self.assertIn('BOTFEATURE view entity=', FIXTURE_GSC)
        self.assertIn('" state=" + b.sessionstate + " angles="', FIXTURE_GSC)
        self.assertIn('botTest_LogViews(sample * 4 + quarter);', FIXTURE_GSC)
        self.assertIn('getplayerangles', FIXTURE_GSC)


def place_log(xs, bots=3):
    log = ''
    for sample, x in enumerate(xs):
        for i in range(bots):
            log += f'0:05 BOTFEATURE place entity={i} sample={sample} origin=({x}, {100 * i}, 16.125)\n'
    return log


class BrainWalkTests(unittest.TestCase):
    def passed(self, log, hold=4):
        return all(ok for ok, _ in validate_brain_walk(log, 3, hold))

    def test_a_bot_that_walks_passes(self):
        self.assertTrue(self.passed(place_log([0, 150, 300, 300, 420])))

    def test_a_bot_that_stands_still_fails(self):
        self.assertFalse(self.passed(place_log([0, 20, 40, 60, 80])))

    def test_a_bot_faster_than_a_player_fails(self):
        self.assertFalse(self.passed(place_log([0, 500, 500, 500, 500])))

    def test_missing_samples_fail(self):
        log = place_log([0, 150, 300, 300, 420])
        self.assertFalse(self.passed(log.replace(log.splitlines(keepends=True)[3], '', 1)))
        self.assertFalse(self.passed(''))

    def test_a_respawn_is_no_walk(self):
        log = place_log([0, 150, 2000, 2150, 2300])
        self.assertFalse(self.passed(log))
        held = ''.join(f'0:05 BOTFEATURE held entity={i} sample={s} state={"dead" if s == 1 else "playing"} '
                       'health=0 team=allies class=CLASS_ASSAULT weapon=m16_mp\n'
                       for s in range(5) for i in range(3))
        self.assertTrue(self.passed(held + log))

    def test_fixture_logs_the_place(self):
        self.assertIn('BOTFEATURE place entity=', FIXTURE_GSC)


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


def notice(bot, target, after=300, units=900, off=12, showing=67):
    return (f"bots: {bot} noticed {target} after {after} ms in view, {units} units, "
            f"{off} degrees off centre, {showing}% showing\n")


class BrainNoticeTests(unittest.TestCase):
    def passed(self, output, both_teams=True):
        return all(ok for ok, _ in validate_brain_notices(output, both_teams))

    def test_fair_notices_pass(self):
        self.assertTrue(self.passed(notice(0, 1) + notice(3, 4, after=200, off=40)))
        self.assertTrue(self.passed(''))
        self.assertTrue(self.passed('', both_teams=False))

    def test_a_teammate_or_itself_fails(self):
        self.assertFalse(self.passed(notice(0, 2)))
        self.assertFalse(self.passed(notice(1, 1)))
        # With every bot on one team nobody is an opponent.
        self.assertFalse(self.passed(notice(0, 1), both_teams=False))

    def test_faster_than_the_floor_fails(self):
        self.assertFalse(self.passed(notice(0, 1, after=100)))

    def test_off_screen_or_hidden_fails(self):
        self.assertFalse(self.passed(notice(0, 1, off=60)))
        self.assertFalse(self.passed(notice(0, 1, showing=0)))


def fired(bot, target, after=150, view=450, units=700):
    return (f'bots: {bot} fired at {target}, {after} ms after noticing, {view} ms after it came into view, '
            f'{units} units\n')


class BrainFightTests(unittest.TestCase):
    def passed(self, output, both_teams=True):
        return all(ok for ok, _ in validate_brain_fights(output, both_teams))

    def test_fair_first_shots_pass(self):
        self.assertTrue(self.passed(fired(0, 1) + fired(3, 4, after=60, view=260)))

    def test_bots_on_both_teams_must_fight(self):
        self.assertFalse(self.passed(''))
        self.assertTrue(self.passed('', both_teams=False))

    def test_a_teammate_fails(self):
        self.assertFalse(self.passed(fired(0, 2)))
        self.assertFalse(self.passed(fired(0, 1), both_teams=False))

    def test_faster_than_a_human_fails(self):
        self.assertFalse(self.passed(fired(0, 1, after=40)))
        self.assertFalse(self.passed(fired(0, 1, view=250)))


class DecisionLogTests(unittest.TestCase):
    def test_cover_and_hunt_lines_are_counted(self):
        output = ('bots: 3 takes cover 210 units away (reloading)\n'
                  'bots: 4 takes cover 0 units away (hurt)\n'
                  'bots: 5 hunts (heard)\nbots: 6 hunts (seen)\nbots: 7 hunts (hit)\n'
                  'bots: 8 hunts (guessed)\n')
        self.assertEqual(len(COVER_ROW.findall(output)), 2)
        self.assertEqual(len(HUNT_ROW.findall(output)), 3)


class CombatHoldTests(unittest.TestCase):
    def test_a_bot_may_die_after_the_first_sample(self):
        log = healthy_log().replace('sample=2 state=playing health=100', 'sample=2 state=dead health=0', 1)
        self.assertFalse(all(ok for ok, _ in validate_hold(log, 3, 2)))
        self.assertTrue(all(ok for ok, _ in validate_hold(log, 3, 2, combat=True)))
        first = healthy_log().replace('sample=0 state=playing health=100', 'sample=0 state=dead health=0', 1)
        self.assertFalse(all(ok for ok, _ in validate_hold(first, 3, 2, combat=True)))


class TeamCapacityTests(unittest.TestCase):
    # A stock team holds half of sv_maxclients (8), so PC bot brain run 1's
    # 6 and 8 bots all sent to allies left 2 and 4 of them spectators.
    def test_more_bots_than_a_team_holds_use_both_teams(self):
        self.assertFalse(uses_both_teams(TEAM_CAPACITY, 0))
        self.assertTrue(uses_both_teams(TEAM_CAPACITY + 1, 0))
        self.assertTrue(uses_both_teams(1, 1))
        self.assertGreaterEqual(MAX_BOTS, 18)

    def test_both_teams_split_by_entity_number(self):
        for fixture in (FIXTURE_GSC, transition_fixture()):
            split = balanced_teams(fixture)
            self.assertNotIn('"{team}"', split)
            self.assertIn('team = "axis";', split)
            self.assertIn('getentitynumber() % 2) == 1', split)
        self.assertIn('team = "axis";', refill_fixture())

    def test_split_teams_are_what_the_hold_expects(self):
        log = ''.join(f'0:01 BOTFEATURE spawned entity={i}\n' for i in range(6))
        for i in range(6):
            team = 'axis' if i % 2 else 'allies'
            log += (f'0:05 BOTFEATURE held entity={i} sample=0 state=playing '
                    f'health=100 team={team} class=CLASS_ASSAULT weapon=m16_mp\n')
        self.assertTrue(all(ok for ok, _ in validate_hold(log, 6, 0, True)))
        self.assertFalse(all(ok for ok, _ in validate_hold(log, 6, 0, False)))


if __name__ == '__main__':
    unittest.main()
