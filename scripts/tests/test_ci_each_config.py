#!/usr/bin/env python3
"""scripts/ci/each-config.sh: both configurations run, and a failure is kept."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/ci/each-config.sh'


def run(commands, *arguments):
    with tempfile.TemporaryDirectory() as temp:
        result = subprocess.run(['bash', str(SCRIPT), *arguments], input=commands, cwd=temp,
                                capture_output=True, text=True, timeout=60)
        files = {path.name: path.read_text() for path in Path(temp).iterdir()}
    return result, files


class EachConfigTests(unittest.TestCase):
    def test_both_configurations_run_in_order_in_a_log_group_each(self):
        result, _ = run('echo "building $config"\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ['::group::Debug', 'building Debug', '::endgroup::',
                          '::group::RelWithDebInfo', 'building RelWithDebInfo', '::endgroup::'])

    def test_the_second_configuration_runs_after_the_first_fails(self):
        result, files = run('touch "ran-$config"\n[[ "$config" != Debug ]]\n')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(sorted(files), ['ran-Debug', 'ran-RelWithDebInfo'])
        self.assertIn('::error::failed in Debug', result.stdout.splitlines())

    def test_a_failure_in_the_second_configuration_fails_the_step(self):
        result, _ = run('[[ "$config" != RelWithDebInfo ]]\n')
        self.assertEqual(result.returncode, 1)
        self.assertIn('::error::failed in RelWithDebInfo', result.stdout.splitlines())

    def test_both_failures_are_named(self):
        result, _ = run('false\n')
        self.assertEqual(result.returncode, 1)
        self.assertIn('::error::failed in Debug RelWithDebInfo', result.stdout.splitlines())

    def test_a_failed_command_stops_that_configuration_at_once(self):
        # errexit holds inside the commands: nothing after a failure runs.
        result, files = run('false\ntouch "after-$config"\n')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(files, {})

    def test_a_failure_before_a_pipe_fails_the_configuration(self):
        # pipefail holds inside the commands, so `runner | tee log` keeps the
        # runner's status.
        result, files = run('{ echo "$config output"; false; } | tee "$config.log"\n')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(files, {'Debug.log': 'Debug output\n',
                                 'RelWithDebInfo.log': 'RelWithDebInfo output\n'})

    def test_an_unset_variable_fails_the_configuration(self):
        result, _ = run('echo "$not_set_anywhere"\n')
        self.assertEqual(result.returncode, 1)

    def test_the_commands_cannot_read_the_rest_of_standard_input(self):
        result, _ = run('cat\necho "after $config"\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('after RelWithDebInfo', result.stdout)

    def test_no_commands_is_an_error_not_a_pass(self):
        for commands in ('', '  \n\n'):
            with self.subTest(commands=commands):
                result, _ = run(commands)
                self.assertEqual(result.returncode, 2)
                self.assertIn('no commands', result.stderr)

    def test_arguments_are_refused(self):
        result, _ = run('true\n', 'Debug')
        self.assertEqual(result.returncode, 2)
        self.assertIn('takes no arguments', result.stderr)


if __name__ == '__main__':
    unittest.main()
