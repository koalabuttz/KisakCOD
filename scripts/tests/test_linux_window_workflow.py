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
        self.assertEqual(self.job['strategy']['matrix']['sdl'], ['2', '3'])

    def test_sdl3_sdk_is_pinned_and_32_bit(self):
        step = next(step for step in self.job['steps']
                    if step.get('name') == 'Build pinned 32-bit SDL3 SDK')
        self.assertEqual(step['if'], 'matrix.sdl == 3')
        self.assertIn('877399b2b2cf21e67554ed9046410f268ce1d1b2', step['run'])
        for flag in ('-DCMAKE_C_FLAGS=-m32', '-DCMAKE_CXX_FLAGS=-m32',
                     '-DCMAKE_SHARED_LINKER_FLAGS=-m32', '-DSDL_SHARED=ON',
                     '-DSDL_X11=OFF', '-DSDL_WAYLAND=OFF'):
            self.assertIn(flag, step['run'])
        self.assertIn('grep -q ELF32', step['run'])

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
        self.assertIn('window-sdl${{ matrix.sdl }}-${{ matrix.config }}', step['run'])
        self.assertIn('--sdl-major "${{ matrix.sdl }}"', step['run'])
        self.assertIn('--sdl3-dir "$RUNNER_TEMP/SDL3-sdk/lib/cmake/SDL3"', step['run'])
        self.assertIn('export LD_LIBRARY_PATH=', step['run'])
        self.assertNotIn('--bundle', step['run'])

    def test_failure_logs_are_retained(self):
        upload = next(step for step in self.job['steps']
                      if step.get('uses', '').startswith('actions/upload-artifact@'))
        self.assertEqual(upload['if'], 'always()')
        self.assertEqual(upload['with']['path'], 'artifacts/*.log')
        self.assertIn('sdl${{ matrix.sdl }}', upload['with']['name'])


class AssetFreeWorldSurfaceWorkflowTests(unittest.TestCase):
    """The asset-free job: no DXVK, no SDL, no GPU, no game assets, no CMake."""

    JOB = 'world_surface'

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        cls.job = cls.workflow['jobs'][cls.JOB]
        cls.run_blocks = [step['run'] for step in cls.job['steps'] if 'run' in step]
        cls.commands = '\n'.join(cls.run_blocks)

    def test_path_triggers_reach_both_test_trees(self):
        for event in ('push', 'pull_request'):
            paths = self.workflow['on'][event]['paths']
            for path in ('tests/linux_renderer/**', 'tests/linux_asset_export/**'):
                with self.subTest(event=event, path=path):
                    self.assertIn(path, paths)

    def test_exactly_two_explicit_configurations(self):
        self.assertEqual(self.job['strategy']['matrix']['config'], ['Debug', 'RelWithDebInfo'])
        self.assertEqual(self.job['runs-on'], 'ubuntu-24.04')
        self.assertIn('timeout-minutes', self.job)
        # One job, not one per configuration plus a variant.
        self.assertNotIn('sdl', self.job['strategy']['matrix'])

    def test_the_job_installs_only_the_multilib_c_plus_plus_toolchain(self):
        install = next(step for step in self.job['steps']
                       if step.get('name') == 'Install 32-bit C++ toolchain')
        self.assertIn('g++-multilib', install['run'])
        self.assertIn('g++', install['run'])
        self.assertIn('python3', install['run'])
        self.assertNotIn('dpkg --add-architecture', install['run'])
        for package in ('libsdl', 'dxvk', 'vulkan', 'mesa', 'xvfb', 'libgl'):
            self.assertNotIn(package, install['run'].lower())

    def test_no_step_needs_a_graphics_stack_or_game_data(self):
        for step in self.job['steps']:
            text = (step.get('name', '') + '\n' + step.get('run', '')).lower()
            for forbidden in ('dxvk', 'sdl', 'vulkan', 'xvfb', 'glx', 'vulkaninfo',
                              'steam', 'iw_', 'zone', 'main/'):
                with self.subTest(step=step.get('name'), forbidden=forbidden):
                    self.assertNotIn(forbidden, text)
            self.assertNotIn('SDL_VIDEODRIVER', step.get('env', {}))
            self.assertNotIn('SDL_AUDIODRIVER', step.get('env', {}))

    def test_the_32_bit_target_is_probed_before_the_checks_run(self):
        step = next(step for step in self.job['steps']
                    if step.get('name') == 'Verify the 32-bit C++20 toolchain')
        self.assertIn('g++ -m32 -std=c++20', step['run'])
        self.assertIn('gcc --version', step['run'])
        # The probe comes before the runner so a missing multilib fails fast and
        # says why, instead of appearing as a compile error deep in a test.
        names = [entry.get('name') for entry in self.job['steps']]
        self.assertLess(names.index('Verify the 32-bit C++20 toolchain'),
                        names.index('Run asset-free world surface checks'))

    def test_the_runner_contract_test_gates_the_check_run(self):
        names = [step.get('name') for step in self.job['steps']]
        self.assertLess(names.index('Check world surface runner configuration and failure handling'),
                        names.index('Run asset-free world surface checks'))
        self.assertIn('python3 scripts/tests/test_world_surface_check_runner.py', self.commands)

    def test_the_runner_is_invoked_per_configuration_with_pipefail(self):
        step = next(step for step in self.job['steps']
                    if step.get('name') == 'Run asset-free world surface checks')
        self.assertIn('scripts/tests/run_world_surface_checks.py', step['run'])
        self.assertIn('--config "${{ matrix.config }}"', step['run'])
        self.assertIn('--cxx g++', step['run'])
        self.assertIn('--log-dir "artifacts/world-surface-${{ matrix.config }}"', step['run'])
        self.assertIn('python3 -u', step['run'])
        # tee alone would hide the runner's exit status; pipefail carries it.
        self.assertIn('2>&1 | tee', step['run'])
        self.assertIn('set -euo pipefail', step['run'])

    def test_every_command_block_propagates_failure(self):
        # A run block without these would let a failed compile or check pass the job.
        for step in self.job['steps']:
            if 'run' not in step:
                continue
            with self.subTest(step=step.get('name')):
                self.assertTrue(step['run'].lstrip().startswith('set -euo pipefail'),
                                step.get('name'))

    def test_no_step_configures_or_builds_the_engine(self):
        for forbidden in ('cmake -S', 'cmake --build', 'build-linux.sh', 'ninja',
                          'pkg-config', 'pkg_check_modules', 'compile_commands.json'):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.commands)

    def test_check_logs_and_the_summary_are_kept(self):
        upload = next(step for step in self.job['steps']
                      if step.get('uses', '').startswith('actions/upload-artifact@'))
        self.assertEqual(upload['if'], 'always()')
        self.assertIn('world-surface-${{ matrix.config }}', upload['with']['name'])
        self.assertEqual(upload['with']['path'], 'artifacts/')
        self.assertEqual(upload['with']['if-no-files-found'], 'warn')

    def test_the_graphical_window_job_is_untouched_by_this_one(self):
        # The existing window job keeps its own two SDL variants and its pinned SDK
        # step; the asset-free job must not become a second copy of it.
        window = self.workflow['jobs']['window']
        self.assertEqual(window['strategy']['matrix']['sdl'], ['2', '3'])
        window_commands = '\n'.join(step.get('run', '') for step in window['steps'])
        self.assertIn('Build pinned 32-bit SDL3 SDK', [step.get('name') for step in window['steps']])
        self.assertNotIn('run_world_surface_checks.py', window_commands)
        self.assertNotIn('test_linux_window.py', self.commands)


class AssetFreeFreeCameraWorkflowTests(unittest.TestCase):
    """The free-camera job: Clang -m32 standalone tests, no graphics stack or assets."""

    JOB = 'free_camera'

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        cls.job = cls.workflow['jobs'][cls.JOB]
        cls.commands = '\n'.join(step['run'] for step in cls.job['steps'] if 'run' in step)

    def test_path_triggers_reach_the_test_tree(self):
        for event in ('push', 'pull_request'):
            self.assertIn('tests/linux_free_camera/**', self.workflow['on'][event]['paths'])

    def test_exactly_two_explicit_configurations(self):
        self.assertEqual(self.job['strategy']['matrix']['config'], ['Debug', 'RelWithDebInfo'])
        self.assertEqual(self.job['runs-on'], 'ubuntu-24.04')
        self.assertIn('timeout-minutes', self.job)

    def test_only_the_32_bit_clang_toolchain_is_installed(self):
        install = next(step for step in self.job['steps']
                       if step.get('name') == 'Install 32-bit Clang toolchain')
        for package in ('clang', 'g++-multilib', 'python3'):
            self.assertIn(package, install['run'])
        for package in ('libsdl', 'dxvk', 'vulkan', 'mesa', 'xvfb', 'libgl'):
            self.assertNotIn(package, install['run'].lower())

    def test_no_step_needs_a_graphics_stack_game_data_or_engine_build(self):
        for step in self.job['steps']:
            text = (step.get('name', '') + '\n' + step.get('run', '')).lower()
            for forbidden in ('dxvk', 'sdl', 'vulkan', 'xvfb', 'steam', 'zone', 'main/',
                              'cmake', 'build-linux.sh', 'ninja', 'pkg-config'):
                with self.subTest(step=step.get('name'), forbidden=forbidden):
                    self.assertNotIn(forbidden, text)

    def test_probe_and_contract_test_gate_the_run(self):
        names = [step.get('name') for step in self.job['steps']]
        run = names.index('Run asset-free free camera checks')
        self.assertLess(names.index('Verify the 32-bit C++20 toolchain'), run)
        self.assertLess(names.index('Check free camera runner configuration and failure handling'), run)
        self.assertIn('python3 scripts/tests/test_free_camera_check_runner.py', self.commands)

    def test_runner_invocation_carries_its_exit_status(self):
        step = next(step for step in self.job['steps']
                    if step.get('name') == 'Run asset-free free camera checks')
        for part in ('scripts/tests/run_free_camera_checks.py', '--config "${{ matrix.config }}"',
                     '--cxx clang++', '--log-dir "artifacts/free-camera-${{ matrix.config }}"',
                     'python3 -u', '2>&1 | tee'):
            self.assertIn(part, step['run'])
        for step in self.job['steps']:
            if 'run' in step:
                self.assertTrue(step['run'].lstrip().startswith('set -euo pipefail'), step.get('name'))

    def test_logs_are_kept(self):
        upload = next(step for step in self.job['steps']
                      if step.get('uses', '').startswith('actions/upload-artifact@'))
        self.assertEqual(upload['if'], 'always()')
        self.assertIn('free-camera-${{ matrix.config }}', upload['with']['name'])
        self.assertEqual(upload['with']['path'], 'artifacts/')


if __name__ == '__main__':
    unittest.main()
