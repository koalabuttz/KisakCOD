#!/usr/bin/env python3
"""Static workflow contract; does not claim hosted execution or graphical coverage."""
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class WindowWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # BaseLoader preserves GitHub's 'on' key rather than YAML 1.1 booleans.
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        cls.job = cls.workflow['jobs']['window']

    def test_path_triggers_cover_window_and_command_fixtures(self):
        for event in ('push', 'pull_request'):
            paths = self.workflow['on'][event]['paths']
            for path in ('src/**', 'scripts/**', 'tests/linux_window/**',
                         'tests/linux_client_commands/**', 'tests/linux_client_core/**'):
                with self.subTest(event=event, path=path):
                    self.assertIn(path, paths)

    def test_configuration_matrix(self):
        self.assertEqual(self.job['strategy']['matrix']['config'],
                         ['Debug', 'RelWithDebInfo'])
        self.assertEqual(self.job['runs-on'], 'ubuntu-24.04')

    def test_matching_i386_sdk_is_explicit(self):
        commands = '\n'.join(step.get('run', '') for step in self.job['steps'])
        self.assertIn('dpkg --add-architecture i386', commands)
        self.assertIn('libsdl2-dev:i386', commands)
        self.assertIn('--sdl2-dir /usr/lib/i386-linux-gnu/cmake/SDL2', commands)

    def test_existing_checker_runs_with_dummy_drivers_and_failure_propagation(self):
        step = next(step for step in self.job['steps']
                    if 'test_linux_window.py' in step.get('run', ''))
        self.assertEqual(step['env']['SDL_VIDEODRIVER'], 'dummy')
        self.assertEqual(step['env']['SDL_AUDIODRIVER'], 'dummy')
        self.assertIn('set -euo pipefail', step['run'])
        self.assertIn('--video-driver dummy', step['run'])
        self.assertIn('window-${{ matrix.config }}', step['run'])
        self.assertNotIn('--bundle', step['run'])

    def test_failure_logs_are_retained(self):
        upload = next(step for step in self.job['steps']
                      if step.get('uses', '').startswith('actions/upload-artifact@'))
        self.assertEqual(upload['if'], 'always()')
        self.assertEqual(upload['with']['path'], 'artifacts/window-${{ matrix.config }}.log')


if __name__ == '__main__':
    unittest.main()
