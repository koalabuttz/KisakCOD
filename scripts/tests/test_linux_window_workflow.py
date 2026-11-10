#!/usr/bin/env python3
"""Static workflow contract; does not claim hosted execution or graphical coverage."""
from pathlib import Path
import json
import re
import subprocess
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]

# Pull requests run only the SDL3 window jobs; pushes to master and manual runs
# also run SDL2 (CiTriggerTests checks what the expression gives).
WINDOW_SDL_MATRIX = "${{ fromJSON(github.event_name == 'pull_request' && '[3]' || '[2, 3]') }}"


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
        self.assertEqual(self.job['strategy']['matrix']['sdl'], WINDOW_SDL_MATRIX)

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


def each_config_commands(test, job, step_name):
    """The commands a step hands to scripts/ci/each-config.sh, which runs them
    for Debug and then RelWithDebInfo in the one job (its own test,
    test_ci_each_config.py, checks that a failure in either is kept)."""
    run = next(step['run'] for step in job['steps'] if step.get('name') == step_name)
    test.assertNotIn('strategy', job)
    test.assertNotIn('matrix.', run)
    test.assertTrue(run.lstrip().startswith('set -euo pipefail'))
    lines = run.splitlines()
    start = lines.index("scripts/ci/each-config.sh <<'COMMANDS'")
    end = lines.index('COMMANDS')
    test.assertLess(start, end)
    # Nothing after the list could change the step's status.
    test.assertEqual([line for line in lines[end + 1:] if line.strip()], [])
    return '\n'.join(lines[start + 1:end])


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
        # One job runs both configurations, so the pair needs one runner.
        each_config_commands(self, self.job, 'Run asset-free world surface checks')
        self.assertEqual(self.job['runs-on'], 'ubuntu-24.04')
        self.assertIn('timeout-minutes', self.job)

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
        commands = each_config_commands(self, self.job, 'Run asset-free world surface checks')
        self.assertIn('scripts/tests/run_world_surface_checks.py', commands)
        self.assertIn('--config "$config"', commands)
        self.assertIn('--cxx g++', commands)
        self.assertIn('--log-dir "artifacts/world-surface-$config"', commands)
        self.assertIn('python3 -u', commands)
        # tee alone would hide the runner's exit status; the pipefail that
        # each-config.sh sets carries it.
        self.assertIn('2>&1 | tee', commands)

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
        self.assertEqual(upload['with']['name'], 'KisakCOD-world-surface')
        self.assertEqual(upload['with']['path'], 'artifacts/')
        self.assertEqual(upload['with']['if-no-files-found'], 'warn')

    def test_the_graphical_window_job_is_untouched_by_this_one(self):
        # The existing window job keeps its own two SDL variants and its pinned SDK
        # step; the asset-free job must not become a second copy of it.
        window = self.workflow['jobs']['window']
        self.assertEqual(window['strategy']['matrix']['sdl'], WINDOW_SDL_MATRIX)
        window_commands = '\n'.join(step.get('run', '') for step in window['steps'])
        self.assertIn('Build pinned 32-bit SDL3 SDK', [step.get('name') for step in window['steps']])
        self.assertNotIn('run_world_surface_checks.py', window_commands)
        self.assertNotIn('test_linux_window.py', self.commands)


class NativeD3D9BoundaryWorkflowTests(unittest.TestCase):
    """The opt-in D3D9 boundary job: pinned DXVK headers only, no device, no assets."""

    JOB = 'native_d3d9_boundary'
    DXVK_REVISION = 'c3dd74be6baec53786d4e064a572185b70347a17'
    DIRECTX_REVISION = '9df86f2341616ef1888ae59919feaa6d4fad693d'

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        cls.job = cls.workflow['jobs'][cls.JOB]
        cls.names = [step.get('name') for step in cls.job['steps']]
        cls.commands = '\n'.join(step.get('run', '') for step in cls.job['steps'])

    def step(self, name):
        return next(step for step in self.job['steps'] if step.get('name') == name)

    def test_path_triggers_reach_the_boundary_tests(self):
        for event in ('push', 'pull_request'):
            paths = self.workflow['on'][event]['paths']
            for path in ('src/**', 'scripts/**', 'tests/linux_d3d9_boundary/**'):
                with self.subTest(event=event, path=path):
                    self.assertIn(path, paths)

    def test_exactly_two_explicit_configurations(self):
        self.assertEqual(self.job['strategy']['matrix']['config'], ['Debug', 'RelWithDebInfo'])
        self.assertEqual(self.job['runs-on'], 'ubuntu-24.04')
        self.assertIn('timeout-minutes', self.job)
        self.assertEqual(self.job['strategy']['fail-fast'], 'false')

    def test_dxvk_and_its_directx_headers_are_pinned_and_verified(self):
        self.assertEqual(self.job['env']['DXVK_REVISION'], self.DXVK_REVISION)
        self.assertEqual(self.job['env']['DXVK_DIRECTX_HEADERS_REVISION'], self.DIRECTX_REVISION)
        fetch = self.step('Fetch pinned DXVK Native 2.7.1 headers')['run']
        self.assertIn('test "$(git -C "$src" rev-parse HEAD)" = "$DXVK_REVISION"', fetch)
        self.assertIn('ls-tree HEAD include/native/directx', fetch)
        self.assertIn('test "$recorded" = "$DXVK_DIRECTX_HEADERS_REVISION"', fetch)
        self.assertIn('test "$(git -C "$headers" rev-parse HEAD)" = "$DXVK_DIRECTX_HEADERS_REVISION"', fetch)
        self.assertIn("version : '2.7.1'", fetch)

    def test_dxvk_is_never_built_and_no_graphics_stack_or_game_data_is_used(self):
        for step in self.job['steps']:
            text = step.get('run', '').lower()
            for forbidden in ('meson setup', 'meson compile', 'ninja -c', 'libdxvk', 'vulkan', 'libsdl', 'xvfb', 'mesa',
                              'steam', 'iw_', 'main/', 'dpkg --add-architecture'):
                with self.subTest(step=step.get('name'), forbidden=forbidden):
                    self.assertNotIn(forbidden, text)
            self.assertNotIn('SDL_VIDEODRIVER', step.get('env', {}))
            self.assertNotIn('DXVK_WSI_DRIVER', step.get('env', {}))
        self.assertNotIn('KISAK_DXVK_NATIVE_LIBRARY', self.commands)

    def test_toolchains_are_probed_before_any_check(self):
        probe = self.step('Verify the 32-bit toolchains')['run']
        self.assertIn('for cxx in g++ clang++', probe)
        self.assertIn('-m32 -std=c++20', probe)
        self.assertIn('objdump --version', probe)
        self.assertLess(self.names.index('Verify the 32-bit toolchains'),
                        self.names.index('Run asset-free native D3D9 boundary checks'))

    def test_runner_contract_gates_the_run(self):
        self.assertLess(self.names.index('Check native D3D9 runner configuration and failure handling'),
                        self.names.index('Run asset-free native D3D9 boundary checks'))
        self.assertIn('python3 scripts/tests/test_native_d3d9_boundary_runner.py', self.commands)

    def test_runner_is_invoked_per_configuration_with_both_compilers_and_pipefail(self):
        run = self.step('Run asset-free native D3D9 boundary checks')['run']
        self.assertIn('scripts/tests/run_native_d3d9_boundary_checks.py', run)
        self.assertIn('--config "${{ matrix.config }}"', run)
        self.assertIn('--dxvk-source "$RUNNER_TEMP/dxvk-source"', run)
        self.assertIn('--header-cxx g++ --header-cxx clang++ --engine-cxx clang++', run)
        self.assertIn('--log-dir "artifacts/native-d3d9-${{ matrix.config }}"', run)
        self.assertIn('2>&1 | tee', run)

    def test_cmake_proofs_are_opt_in_and_never_build_the_server(self):
        cmake = self.step('Configure and build the opt-in CMake boundary proofs')['run']
        self.assertIn('-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON', cmake)
        self.assertIn('-DKISAK_DXVK_NATIVE_INCLUDE_DIR="$RUNNER_TEMP/dxvk-include/dxvk"', cmake)
        self.assertIn('--stage-dxvk-headers "$RUNNER_TEMP/dxvk-include/dxvk"', cmake)
        self.assertIn('--target KisakCOD-native-d3d9-proofs', cmake)
        self.assertNotIn('KisakCOD-dedi', cmake)
        self.assertIn('diff <(tail -n +2', cmake)
        self.assertIn("grep -qx 'PASS engine D3D9 layouts'", cmake)

    def test_world_frontend_probe_is_linked_inspected_and_never_run(self):
        # The offline probe builds on the boundary's configure, after its proofs. It
        # needs a game install to run, so CI only links it, runs its asset-free
        # invariants test and checks what the executable depends on.
        name = 'Build the offline world front-end probe'
        self.assertLess(self.names.index('Configure and build the opt-in CMake boundary proofs'),
                        self.names.index(name))
        build = self.step(name)['run']
        self.assertIn('-DKISAK_BUILD_WORLD_FRONTEND_PROBE=ON', build)
        self.assertIn('--target KisakCOD-world-frontend-probe KisakCOD-world-frontend-invariants-test', build)
        self.assertIn('"$out/KisakCOD-world-frontend-invariants-test"', build)
        self.assertIn("Class:[[:space:]]+ELF32", build)
        self.assertIn("Machine:[[:space:]]+Intel 80386", build)
        self.assertIn('Shared library', build)
        # Never run: no map, no game path, no output directory for an export, and
        # no command line that starts with the probe itself.
        for forbidden in ('KISAK_WORLD_FRONTEND_MAP', 'KISAK_UI_ASSET_EXPORT_DIR', 'fs_basepath'):
            self.assertNotIn(forbidden, build)
        for line in build.splitlines():
            self.assertFalse(line.strip().startswith(('"$probe"', '$probe')), line)
        self.assertNotIn('KisakCOD-dedi', build)

    def test_every_command_block_propagates_failure(self):
        for step in self.job['steps']:
            if 'run' not in step:
                continue
            with self.subTest(step=step.get('name')):
                self.assertTrue(step['run'].lstrip().startswith('set -euo pipefail'), step.get('name'))

    def test_logs_are_kept_and_no_binary_is_uploaded(self):
        upload = next(step for step in self.job['steps']
                      if step.get('uses', '').startswith('actions/upload-artifact@'))
        self.assertEqual(upload['if'], 'always()')
        self.assertEqual(upload['with']['path'], 'artifacts/')
        self.assertIn('native-d3d9-${{ matrix.config }}', upload['with']['name'])

    # Asset-free renderer tests that compile production units through the boundary
    # with DXVK's headers (never its library). Every other job stays DXVK-free.
    HEADER_CONSUMERS = ('world_frontend',)

    # Jobs that turn the boundary's CMake option on. device_probe does it to link
    # the device probe against a stand-in D3D9 library (DeviceProbeWorkflowTests);
    # x64_client and arm64_client do it to build the 64-bit multiplayer clients
    # against stand-ins; vulkan_window does it to compile the Vulkan D3D9 front
    # against the renderer's D3D9 declarations, linking no D3D9 library
    # (VulkanWindowWorkflowTests); vulkan_engine does it to build the engine's
    # programs with the Vulkan back end against the stand-ins
    # (VulkanEngineWorkflowTests). The PS Vita client, which builds through
    # the boundary too, is in build-kisakcod-vita.yaml.
    BOUNDARY_JOBS = ('native_d3d9_boundary', 'device_probe', 'x64_client', 'arm64_client', 'vulkan_window',
                     'vulkan_engine')

    def test_other_jobs_keep_their_defaults(self):
        # The boundary stays opt-in: no other job turns the CMake option on, and
        # only the named header consumers fetch DXVK at all.
        for name, job in self.workflow['jobs'].items():
            if name in self.BOUNDARY_JOBS:
                continue
            text = '\n'.join(step.get('run', '') for step in job['steps'])
            with self.subTest(job=name):
                self.assertNotIn('KISAK_BUILD_LINUX_NATIVE_D3D9', text)
                if name not in self.HEADER_CONSUMERS:
                    self.assertNotIn('dxvk', text.lower())

    def test_header_consumers_use_the_same_pins_and_build_nothing(self):
        for name in self.HEADER_CONSUMERS:
            job = self.workflow['jobs'][name]
            text = '\n'.join(step.get('run', '') for step in job['steps']).lower()
            with self.subTest(job=name):
                for key in ('DXVK_REVISION', 'DXVK_DIRECTX_HEADERS_REVISION'):
                    self.assertEqual(job['env'][key], self.job['env'][key])
                for forbidden in ('meson setup', 'ninja', 'libdxvk', 'cmake --build', '-dkisak_dxvk_native'):
                    self.assertNotIn(forbidden, text)



class DeviceProbeWorkflowTests(unittest.TestCase):
    """The device probe without a GPU: a link proof and its fail-closed run."""

    JOB = 'device_probe'
    RUN_STEP = "Run the Win32 window layer test and the probe's fail-closed path"
    BUILD_STEP = 'Build the device probe against the stand-in D3D9 library'
    WORLD_BUILD_STEP = 'Build the world device probe against the stand-in D3D9 library'
    WORLD_RUN_STEP = "Run the world device probe's fail-closed paths"
    DRAW_BUILD_STEP = 'Build the world draw probe against the stand-in D3D9 library'
    DRAW_RUN_STEP = "Run the world draw probe's fail-closed paths"
    MP_BUILD_STEP = 'Build the multiplayer client against the stand-in D3D9 and OpenAL libraries'
    MP_RUN_STEP = "Run the multiplayer client's fail-closed start"

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        cls.job = cls.workflow['jobs'][cls.JOB]
        cls.names = [step.get('name') for step in cls.job['steps']]
        cls.commands = '\n'.join(step.get('run', '') for step in cls.job['steps'])

    def step(self, name):
        return next(step for step in self.job['steps'] if step.get('name') == name)

    def test_path_triggers_reach_the_stand_in_library(self):
        for event in ('push', 'pull_request'):
            paths = self.workflow['on'][event]['paths']
            for path in ('src/**', 'scripts/**', 'tests/linux_window/**', 'tests/linux_device_probe/**',
                         'tests/linux_mp_client/**'):
                with self.subTest(event=event, path=path):
                    self.assertIn(path, paths)

    def test_exactly_two_explicit_configurations(self):
        self.assertEqual(self.job['strategy']['matrix']['config'], ['Debug', 'RelWithDebInfo'])
        self.assertEqual(self.job['runs-on'], 'ubuntu-24.04')
        self.assertIn('timeout-minutes', self.job)
        self.assertEqual(self.job['strategy']['fail-fast'], 'false')

    def test_pins_match_the_window_and_boundary_jobs(self):
        boundary = self.workflow['jobs']['native_d3d9_boundary']
        for key in ('DXVK_REVISION', 'DXVK_DIRECTX_HEADERS_REVISION'):
            self.assertEqual(self.job['env'][key], boundary['env'][key])
        self.assertEqual(self.job['env']['SDL3_REVISION'], '877399b2b2cf21e67554ed9046410f268ce1d1b2')
        window_sdl = next(step for step in self.workflow['jobs']['window']['steps']
                          if step.get('name') == 'Build pinned 32-bit SDL3 SDK')['run']
        self.assertIn(self.job['env']['SDL3_REVISION'], window_sdl)
        sdl = self.step('Build pinned 32-bit SDL3 SDK')['run']
        self.assertIn('test "$(git -C "$RUNNER_TEMP/SDL3-source" rev-parse HEAD)" = "$SDL3_REVISION"', sdl)
        for flag in ('-DCMAKE_C_FLAGS=-m32', '-DSDL_SHARED=ON', '-DSDL_X11=OFF', '-DSDL_WAYLAND=OFF'):
            self.assertIn(flag, sdl)
        self.assertIn('grep -q ELF32', sdl)
        fetch = self.step('Fetch pinned DXVK Native 2.7.1 headers')['run']
        self.assertIn('test "$(git -C "$src" rev-parse HEAD)" = "$DXVK_REVISION"', fetch)
        self.assertIn('test "$recorded" = "$DXVK_DIRECTX_HEADERS_REVISION"', fetch)
        self.assertIn('--stage-dxvk-headers "$RUNNER_TEMP/dxvk-include/dxvk"', fetch)

    def test_no_dxvk_build_no_gpu_stack_and_no_game_data(self):
        for step in self.job['steps']:
            text = step.get('run', '').lower()
            for forbidden in ('meson setup', 'meson compile', 'ninja -c', 'libdxvk', 'vulkan', 'mesa', 'lavapipe', 'xvfb', 'steam', 'iw_', 'main/',
                              'fs_basepath', 'kisak_world_frontend_map', 'kisak_ui_asset_export_dir',
                              'kisak_world_draw_map', 'kisak_world_draw_show_seconds',
                              'dpkg --add-architecture', 'kisakcod-dedi'):
                with self.subTest(step=step.get('name'), forbidden=forbidden):
                    self.assertNotIn(forbidden, text)
            self.assertNotIn('DXVK_WSI_DRIVER', step.get('env', {}))

    def test_the_probe_links_only_the_stand_in_d3d9_library(self):
        build = self.step(self.BUILD_STEP)['run']
        self.assertIn('tests/linux_device_probe/d3d9_unavailable.cpp', build)
        self.assertIn('-DKISAK_DXVK_NATIVE_LIBRARY="$stand_in"', build)
        self.assertIn('-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON', build)
        self.assertIn('-DKISAK_BUILD_DEVICE_PROBE=ON', build)
        self.assertIn('-DSDL3_DIR="$RUNNER_TEMP/SDL3-sdk/lib/cmake/SDL3"', build)
        self.assertIn('--target KisakCOD-device-probe KisakCOD-win32-window-test', build)
        self.assertIn('Machine:[[:space:]]+Intel 80386', build)
        self.assertIn("grep -F 'Shared library: [libd3d9_unavailable.so]'", build)
        self.assertIn("the Win32 layer's test must not need the D3D9 library", build)

    def test_the_run_uses_dummy_drivers_and_expects_the_fail_closed_result(self):
        self.assertLess(self.names.index(self.BUILD_STEP), self.names.index(self.RUN_STEP))
        step = self.step(self.RUN_STEP)
        self.assertEqual(step['env']['SDL_VIDEODRIVER'], 'dummy')
        self.assertEqual(step['env']['SDL_AUDIODRIVER'], 'dummy')
        run = step['run']
        self.assertIn('"$out/KisakCOD-win32-window-test"', run)
        self.assertIn('test "$status" -eq 1', run)
        self.assertIn("grep -q '^device_probe=failed stage=direct3d '", run)
        self.assertIn('test "$status" -eq 2', run)

    def test_the_world_device_probe_links_only_the_stand_in_after_the_device_probe(self):
        self.assertLess(self.names.index(self.RUN_STEP), self.names.index(self.WORLD_BUILD_STEP))
        self.assertLess(self.names.index(self.WORLD_BUILD_STEP), self.names.index(self.WORLD_RUN_STEP))
        build = self.step(self.WORLD_BUILD_STEP)['run']
        # The same build directory, so the same stand-in library and SDL3.
        self.assertIn('cmake -S . -B "$BUILD_DIR"', build)
        self.assertIn('-DKISAK_BUILD_WORLD_FRONTEND_PROBE=ON -DKISAK_BUILD_WORLD_DEVICE_PROBE=ON', build)
        self.assertIn('--target KisakCOD-world-device-probe', build)
        self.assertIn('Class:[[:space:]]+ELF32', build)
        self.assertIn('Machine:[[:space:]]+Intel 80386', build)
        self.assertIn("grep -qx 'libSDL3.so.0'", build)
        self.assertIn("grep -qx 'libd3d9_unavailable.so'", build)
        self.assertIn('the world device probe needs a library beyond SDL3, D3D9 and the runtimes', build)

    def test_the_world_device_run_is_the_asset_free_script_on_dummy_drivers(self):
        step = self.step(self.WORLD_RUN_STEP)
        self.assertEqual(step['env']['SDL_VIDEODRIVER'], 'dummy')
        self.assertEqual(step['env']['SDL_AUDIODRIVER'], 'dummy')
        run = step['run']
        self.assertIn('python3 -u scripts/tests/test_world_device_probe.py', run)
        self.assertIn('--probe "$GITHUB_WORKSPACE/out/device-probe-${{ matrix.config }}/KisakCOD-world-device-probe"',
                      run)
        self.assertIn('export LD_LIBRARY_PATH="$RUNNER_TEMP/SDL3-sdk/lib:$RUNNER_TEMP/d3d9-unavailable"', run)

    def test_the_world_device_script_builds_its_own_tree_and_takes_no_game_directory(self):
        script = (ROOT / 'scripts/tests/test_world_device_probe.py').read_text()
        # Its tree is a temporary directory it removes, and the stub is the one
        # file it writes there.
        self.assertIn('tempfile.TemporaryDirectory(prefix="kisak-world-device-")', script)
        self.assertEqual(script.count('.write_text('), 1)
        self.assertIn('"// stub written by the check; no game data\\n"', script)
        # One option names a path, and it is the probe; no game directory argument.
        self.assertEqual(script.count('add_argument('), 2)
        self.assertIn('add_argument("--probe"', script)
        self.assertIn('add_argument("--timeout"', script)
        for forbidden in ('steam', 'game_dir', '.ff"', '.iwd', 'symlink'):
            self.assertNotIn(forbidden, script.lower())
        # The first run stops at the device, before any zone; the refusals are
        # checked by their exact last lines.
        self.assertIn('stage=direct3d: ', script)
        self.assertIn('no zone is queued without a device', script)
        self.assertIn('stage=arguments: ', script)

    def test_the_world_draw_probe_links_only_the_stand_in_after_the_world_device_probe(self):
        self.assertLess(self.names.index(self.WORLD_RUN_STEP), self.names.index(self.DRAW_BUILD_STEP))
        self.assertLess(self.names.index(self.DRAW_BUILD_STEP), self.names.index(self.DRAW_RUN_STEP))
        build = self.step(self.DRAW_BUILD_STEP)['run']
        # The same build directory, so the same stand-in library, SDL3 and the
        # world device probe the draw probe requires.
        self.assertIn('cmake -S . -B "$BUILD_DIR" -DKISAK_BUILD_WORLD_DRAW_PROBE=ON', build)
        self.assertIn('--target KisakCOD-world-draw-probe', build)
        self.assertIn('Class:[[:space:]]+ELF32', build)
        self.assertIn('Machine:[[:space:]]+Intel 80386', build)
        self.assertIn("grep -qx 'libSDL3.so.0'", build)
        self.assertIn("grep -qx 'libd3d9_unavailable.so'", build)
        self.assertIn('the world draw probe needs a library beyond SDL3, D3D9 and the runtimes', build)

    def test_the_world_draw_run_is_the_asset_free_script_on_dummy_drivers(self):
        step = self.step(self.DRAW_RUN_STEP)
        self.assertEqual(step['env']['SDL_VIDEODRIVER'], 'dummy')
        self.assertEqual(step['env']['SDL_AUDIODRIVER'], 'dummy')
        run = step['run']
        self.assertIn('python3 -u scripts/tests/test_world_draw_probe.py', run)
        self.assertIn('--probe "$GITHUB_WORKSPACE/out/device-probe-${{ matrix.config }}/KisakCOD-world-draw-probe"',
                      run)
        self.assertIn('export LD_LIBRARY_PATH="$RUNNER_TEMP/SDL3-sdk/lib:$RUNNER_TEMP/d3d9-unavailable"', run)

    def test_the_world_draw_script_builds_its_own_tree_and_takes_no_game_directory(self):
        script = (ROOT / 'scripts/tests/test_world_draw_probe.py').read_text()
        self.assertIn('tempfile.TemporaryDirectory(prefix="kisak-world-draw-")', script)
        self.assertEqual(script.count('.write_text('), 1)
        self.assertIn('"// stub written by the check; no game data\\n"', script)
        self.assertEqual(script.count('add_argument('), 2)
        self.assertIn('add_argument("--probe"', script)
        self.assertIn('add_argument("--timeout"', script)
        for forbidden in ('steam', 'game_dir', '.ff"', '.iwd', 'symlink'):
            self.assertNotIn(forbidden, script.lower())
        # The first run stops before the renderer starts; the refusals are
        # checked by their exact last lines.
        self.assertIn('stage=direct3d: ', script)
        self.assertIn('R_Init does not run without a Direct3D 9 interface', script)
        self.assertIn('stage=arguments: ', script)

    def test_the_client_links_only_stand_ins_after_the_draw_probe(self):
        self.assertLess(self.names.index(self.DRAW_RUN_STEP), self.names.index(self.MP_BUILD_STEP))
        self.assertLess(self.names.index(self.MP_BUILD_STEP), self.names.index(self.MP_RUN_STEP))
        build = self.step(self.MP_BUILD_STEP)['run']
        # The same build directory, so the same D3D9 stand-in, SDL3 and boundary.
        self.assertIn('tests/linux_mp_client/openal_unavailable.cpp', build)
        self.assertIn('cmake -S . -B "$BUILD_DIR" -DKISAK_BUILD_LINUX_MP_CLIENT=ON -DKISAK_OPENAL_LIBRARY="$openal"',
                      build)
        self.assertIn('--target KisakCOD-mp', build)
        self.assertIn('Class:[[:space:]]+ELF32', build)
        self.assertIn('Machine:[[:space:]]+Intel 80386', build)
        for library in ('libSDL3.so.0', 'libd3d9_unavailable.so', 'libopenal_unavailable.so'):
            self.assertIn(f"grep -qx '{library}'", build)
        self.assertIn('the multiplayer client needs a library beyond SDL3, D3D9, OpenAL and the runtimes', build)

    def test_the_job_installs_the_openal_headers_the_client_sound_code_includes(self):
        # src/sound/snd_local.h includes <AL/al.h>; the library itself is the stand-in.
        install = self.step('Install 32-bit C++ toolchain')['run']
        self.assertIn('libopenal-dev', install)
        self.assertNotIn('libopenal1', install)

    def test_the_client_run_is_the_asset_free_script_on_dummy_drivers(self):
        step = self.step(self.MP_RUN_STEP)
        self.assertEqual(step['env']['SDL_VIDEODRIVER'], 'dummy')
        self.assertEqual(step['env']['SDL_AUDIODRIVER'], 'dummy')
        run = step['run']
        self.assertIn('python3 -u scripts/tests/test_linux_mp_client.py', run)
        self.assertIn('--binary "$GITHUB_WORKSPACE/out/device-probe-${{ matrix.config }}/KisakCOD-mp"', run)
        self.assertIn('export LD_LIBRARY_PATH="$RUNNER_TEMP/SDL3-sdk/lib:$RUNNER_TEMP/d3d9-unavailable:'
                      '$RUNNER_TEMP/openal-unavailable"', run)

    def test_the_game_window_test_is_built_and_runs_before_the_client(self):
        build = self.step(self.MP_BUILD_STEP)['run']
        self.assertIn('--target KisakCOD-mp KisakCOD-mp-client-window-test', build)
        # Linked for its Win32 declarations only: no D3D9 library.
        self.assertIn("readelf -d \"$window_test\" | grep -F 'Shared library: [libSDL3.so.0]'", build)
        self.assertIn('the game window test must not link a D3D9 library', build)
        run = self.step(self.MP_RUN_STEP)['run']
        test = '"$GITHUB_WORKSPACE/out/device-probe-${{ matrix.config }}/KisakCOD-mp-client-window-test"'
        self.assertIn(test, run)
        self.assertLess(run.index(test), run.index('scripts/tests/test_linux_mp_client.py'))

    def test_the_client_script_builds_its_own_trees_and_takes_no_game_directory(self):
        script = (ROOT / 'scripts/tests/test_linux_mp_client.py').read_text()
        self.assertIn('tempfile.TemporaryDirectory(prefix="kisak-mp-client-")', script)
        self.assertEqual(script.count('.write_text('), 1)
        self.assertIn('"// stub written by the check; no game data\\n"', script)
        # The binary, how to run it on another architecture, and the OS its
        # banner names (Linux or macOS); never a game tree.
        self.assertEqual(script.count('add_argument('), 3)
        self.assertIn('add_argument("--binary"', script)
        self.assertIn('add_argument("--emulator"', script)
        self.assertIn('add_argument("--native-os", choices=("Linux", "macOS")', script)
        for forbidden in ('game_dir', '.ff"', '.iwd', 'symlink'):
            self.assertNotIn(forbidden, script.lower())

    def test_every_command_block_propagates_failure(self):
        for step in self.job['steps']:
            if 'run' not in step:
                continue
            with self.subTest(step=step.get('name')):
                self.assertTrue(step['run'].lstrip().startswith('set -euo pipefail'), step.get('name'))

    def test_logs_are_kept_and_no_binary_is_uploaded(self):
        upload = next(step for step in self.job['steps']
                      if step.get('uses', '').startswith('actions/upload-artifact@'))
        self.assertEqual(upload['if'], 'always()')
        self.assertEqual(upload['with']['path'], 'artifacts/')
        self.assertIn('device-probe-${{ matrix.config }}', upload['with']['name'])


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
        each_config_commands(self, self.job, 'Run asset-free free camera checks')
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
        commands = each_config_commands(self, self.job, 'Run asset-free free camera checks')
        for part in ('scripts/tests/run_free_camera_checks.py', '--config "$config"',
                     '--cxx clang++', '--log-dir "artifacts/free-camera-$config"',
                     'python3 -u', '2>&1 | tee'):
            self.assertIn(part, commands)
        for step in self.job['steps']:
            if 'run' in step:
                self.assertTrue(step['run'].lstrip().startswith('set -euo pipefail'), step.get('name'))

    def test_logs_are_kept(self):
        upload = next(step for step in self.job['steps']
                      if step.get('uses', '').startswith('actions/upload-artifact@'))
        self.assertEqual(upload['if'], 'always()')
        self.assertEqual(upload['with']['name'], 'KisakCOD-free-camera')
        self.assertEqual(upload['with']['path'], 'artifacts/')


def _github_glob(pattern):
    """A GitHub Actions path pattern as a regular expression.

    Covers the forms the workflows use: '**' (any characters, '/' included),
    a leading or inner '**/' (zero or more directories), '*' (any characters
    but '/') and literal text.
    """
    out, i = '', 0
    while i < len(pattern):
        if pattern.startswith('**/', i):
            out, i = out + '(?:.*/)?', i + 3
        elif pattern.startswith('**', i):
            out, i = out + '.*', i + 2
        elif pattern[i] == '*':
            out, i = out + '[^/]*', i + 1
        else:
            out, i = out + re.escape(pattern[i]), i + 1
        if pattern[i - 1:i] in '?+[]':
            raise ValueError(f'unsupported pattern character in {pattern!r}')
    return re.compile(out + r'\Z')


def _selected(path, patterns):
    """Whether a changed path triggers a workflow with this 'paths' list:
    patterns apply in order, and a later match overrides an earlier one."""
    selected = False
    for pattern in patterns:
        negated = pattern.startswith('!')
        if _github_glob(pattern[1:] if negated else pattern).match(path):
            selected = not negated
    return selected


class CiTriggerTests(unittest.TestCase):
    """When the Linux and Windows workflows run, and which runs a newer push cancels."""

    # Pull requests: one group per pull request, the newer push cancels. Master:
    # one group, the run in progress finishes and GitHub keeps only the newest
    # run waiting. Manual runs: a group each.
    CONCURRENCY = {
        'group': "${{ github.workflow }}-${{ github.event_name == 'workflow_dispatch' && github.run_id || github.ref }}",
        'cancel-in-progress': "${{ github.event_name == 'pull_request' }}",
    }
    # The PS Vita workflow: every master run finishes too, each in a group of
    # its own, so each merge that changes the port keeps its vpk.
    VITA_CONCURRENCY = {
        'group': "${{ github.workflow }}-${{ github.event_name == 'pull_request' && github.ref || github.run_id }}",
        'cancel-in-progress': 'true',
    }

    # Changes that cannot reach the MSVC builds or the Windows regression checks.
    WINDOWS_SKIPS = (
        'README.md',
        'docs/linux-free-camera.md',
        'docs/history/progress-2026-10-31-client-window.md',
        'src/linux/tools/world_draw_probe.cpp',
        'src/linux/compat.h',
        'tests/linux_compat/msvc_rand_test.cpp',
        'tests/linux_renderer/README.md',
        'scripts/platform/linux/world_draw_probe.cmake',
        'scripts/tests/run_world_surface_checks.py',
        'scripts/tests/test_world_draw_probe.py',
        'scripts/tests/README.md',
        'build-linux.sh',
        '.github/workflows/build-kisakcod-linux.yaml',
        '.github/workflows/build-kisakcod-macos.yaml',
        'src/vita/vita_stats.cpp',
        'src/vita/client/sound/vita_al_output.cpp',
        'src/gfx_gxm/legacy/gxm_legacy.cpp',
        'tests/vita_client/run_plan_test.cpp',
        'tests/vita_gxm/README.md',
        'scripts/platform/vita/CMakeLists.txt',
        'scripts/runs/perf_run.py',
        'scripts/runs/recipes/vulkan-ab-mp-crash.json',
        'tests/runs/fake_game.cpp',
    )
    # Changes that must still build Windows.
    WINDOWS_RUNS = (
        'CMakeLists.txt',
        'src/gfx_d3d/r_image.cpp',
        'src/universal/q_shared.h',
        'src/win32/win_main.cpp',
        'src/linux/client/cinematic/bink_player.cpp',
        'scripts/common_files.cmake',
        'scripts/platform/win32/platform.cmake',
        'scripts/ci/check-windows-bootstrap.ps1',
        'scripts/tests/d3dx/CMakeLists.txt',
        'scripts/tests/test_voice_lifecycle.py',
        'deps/some_library/source.c',
        '.github/workflows/build-kisarcod-win.yaml',
        'src/qcommon/com_frame_log.h',
        'src/gfx_vk/vk_device.cpp',
    )
    # Changes that cannot reach a Mac build, and changes that must still build one.
    MACOS_SKIPS = (
        'README.md',
        'docs/vita.md',
        'src/vita/vita_stats.cpp',
        'src/gfx_gxm/legacy/gxm_legacy.cpp',
        'tests/vita_client/run_plan_test.cpp',
        'scripts/platform/vita/CMakeLists.txt',
        'scripts/runs/perf_run.py',
        'scripts/tests/test_perf_run.py',
    )
    MACOS_RUNS = (
        'CMakeLists.txt',
        'src/qcommon/common.cpp',
        'src/linux/main.cpp',
        'src/gfx_vk/vk_device.cpp',
        'src/gfx_d3d/r_image.cpp',
        'scripts/platform/macos/platform.cmake',
        'scripts/platform/linux/CMakeLists.txt',
        'deps/some_library/source.c',
        '.github/workflows/build-kisakcod-macos.yaml',
    )

    @classmethod
    def setUpClass(cls):
        def load(name):
            return yaml.load((ROOT / '.github/workflows' / name).read_text(),
                             Loader=yaml.BaseLoader)
        cls.linux = load('build-kisakcod-linux.yaml')
        cls.windows = load('build-kisarcod-win.yaml')
        cls.macos = load('build-kisakcod-macos.yaml')
        cls.vita = load('build-kisakcod-vita.yaml')
        cls.windows_paths = cls.windows['on']['pull_request']['paths']

    def test_a_newer_push_cancels_only_the_same_pull_requests_older_run(self):
        for name, workflow in (('linux', self.linux), ('windows', self.windows), ('macos', self.macos)):
            with self.subTest(workflow=name):
                self.assertEqual(workflow['concurrency'], self.CONCURRENCY)
        self.assertEqual(self.vita['concurrency'], self.VITA_CONCURRENCY)

    def test_no_workflow_cancels_a_master_run_in_progress(self):
        # With merges coming faster than the hosted queue, cancelling the older
        # master run meant that none finished: the platforms checked only
        # after a merge went unchecked and master had no vpk.
        for path in sorted((ROOT / '.github/workflows').glob('*.y*ml')):
            workflow = yaml.load(path.read_text(encoding='utf-8'), Loader=yaml.BaseLoader)
            concurrency = workflow.get('concurrency')
            if concurrency is None:
                continue
            with self.subTest(workflow=path.name):
                self.assertIn(concurrency, (self.CONCURRENCY, self.VITA_CONCURRENCY))

    # The PS Vita port's own files: only its workflow builds or reads them.
    VITA_ONLY = (
        'src/vita/vita_main.cpp',
        'src/vita/client/sound/vita_al_output.cpp',
        'src/gfx_gxm/legacy/gxm_legacy.cpp',
        'scripts/platform/vita/mp_client.cmake',
        'scripts/tests/test_vita_run_plan.py',
        'scripts/ci/install-vitasdk.sh',
        'tests/vita_client/run_plan_test.cpp',
        'tests/vita_gxm/heap_test.cpp',
    )
    # Shared with the other builds: both workflows run.
    VITA_AND_LINUX = (
        'src/qcommon/common.cpp',
        'src/linux/sys_assert.cpp',
        'src/gfx_vk/d3d9/vk_d3d9_backend.h',
        'CMakeLists.txt',
        'scripts/ci/apt-install.sh',
        'scripts/platform/linux/CMakeLists.txt',
        'deps/some_library/source.c',
    )

    def test_vita_only_changes_start_the_vita_workflow_alone(self):
        for event in ('push', 'pull_request'):
            for path in self.VITA_ONLY:
                with self.subTest(event=event, path=path):
                    self.assertTrue(_selected(path, self.vita['on'][event]['paths']))
                    self.assertFalse(_selected(path, self.linux['on'][event]['paths']))
                    self.assertFalse(_selected(path, self.windows['on'][event]['paths']))
                    self.assertFalse(_selected(path, self.macos['on'][event]['paths']))
            for path in ('docs/vita.md', 'README.md'):
                with self.subTest(event=event, path=path):
                    self.assertFalse(_selected(path, self.vita['on'][event]['paths']))

    def test_shared_changes_start_the_vita_and_the_linux_workflow(self):
        for event in ('push', 'pull_request'):
            for path in self.VITA_AND_LINUX:
                with self.subTest(event=event, path=path):
                    self.assertTrue(_selected(path, self.vita['on'][event]['paths']))
                    self.assertTrue(_selected(path, self.linux['on'][event]['paths']))
        self.assertEqual(self.vita['on']['push']['branches'], ['master'])
        self.assertEqual(self.vita['on']['pull_request']['branches'], ['master'])
        self.assertIn('workflow_dispatch', self.vita['on'])

    def test_no_linux_job_builds_or_reads_what_only_the_vita_workflow_watches(self):
        # The Linux workflow skips the port's own files, so nothing in it may
        # depend on them: the port's jobs and host tests are in the Vita
        # workflow, and every file under scripts/tests the Vita workflow runs
        # starts it.
        linux_commands = '\n'.join(step.get('run', '')
                                   for job in self.linux['jobs'].values() for step in job['steps'])
        for word in ('test_vita_', 'install-vitasdk', 'KISAK_PLATFORM=vita', 'src/vita', 'src/gfx_gxm',
                     'tests/vita_'):
            with self.subTest(word=word):
                self.assertNotIn(word, linux_commands)
        self.assertEqual(sorted(self.vita['jobs']), ['vita_dedicated', 'vita_mp_client'])
        vita_commands = '\n'.join(step.get('run', '')
                                  for job in self.vita['jobs'].values() for step in job['steps'])
        tracked = subprocess.run(['git', 'ls-files', 'scripts/tests/test_vita_*.py'], cwd=ROOT, check=True,
                                 capture_output=True, text=True).stdout.split()
        self.assertGreaterEqual(len(tracked), 5)
        for path in tracked:
            with self.subTest(test=path):
                self.assertIn(f'python3 {path}', vita_commands)
        self.assertIn('python3 scripts/tools/strip_cdecl.py --check', vita_commands)

    def test_the_linux_workflow_runs_when_the_vita_workflow_changes(self):
        # This file checks the Vita workflow, and the Linux native job runs it.
        for event in ('push', 'pull_request'):
            with self.subTest(event=event):
                self.assertIn('.github/workflows/build-kisakcod-vita.yaml', self.linux['on'][event]['paths'])
                self.assertFalse(_selected('.github/workflows/build-kisakcod-vita.yaml',
                                           self.windows['on'][event]['paths']))

    def test_window_jobs_run_sdl3_on_pull_requests_and_both_after_a_merge(self):
        sdl = self.linux['jobs']['window']['strategy']['matrix']['sdl']
        self.assertEqual(sdl, WINDOW_SDL_MATRIX)
        found = re.fullmatch(
            r"\$\{\{ fromJSON\(github\.event_name == 'pull_request' && '([^']*)' \|\| '([^']*)'\) \}\}",
            sdl)
        self.assertIsNotNone(found)
        self.assertEqual(json.loads(found.group(1)), [3])
        self.assertEqual(json.loads(found.group(2)), [2, 3])

    def test_windows_paths_are_the_same_for_both_events(self):
        self.assertEqual(self.windows['on']['push']['paths'], self.windows_paths)
        self.assertEqual(self.windows['on']['push']['branches'], ['master'])
        self.assertEqual(self.windows['on']['pull_request']['branches'], ['master'])
        self.assertIn('workflow_dispatch', self.windows['on'])

    def test_windows_skips_documentation_and_linux_only_changes(self):
        for path in self.WINDOWS_SKIPS:
            with self.subTest(path=path):
                self.assertFalse(_selected(path, self.windows_paths))

    DRAFT_RULE = "github.event_name != 'pull_request' || github.event.pull_request.draft == false"
    AFTER_MERGE_RULE = "github.event_name != 'pull_request'"
    MACOS_ARCHS = ("${{ fromJSON(github.event_name == 'pull_request' && '[\"arm64\"]' "
                   "|| '[\"arm64\", \"x86_64\"]') }}")

    def test_no_check_runs_while_a_pull_request_is_a_draft(self):
        # Every workflow a pull request starts: its jobs wait while the pull
        # request is a draft, and marking it ready for review starts them.
        seen = 0
        for path in sorted((ROOT / '.github/workflows').glob('*.y*ml')):
            workflow = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
            if 'pull_request' not in workflow['on']:
                continue
            seen += 1
            with self.subTest(workflow=path.name):
                self.assertEqual(workflow['on']['pull_request'].get('types'),
                                 ['opened', 'synchronize', 'reopened', 'ready_for_review'])
                for key, job in workflow['jobs'].items():
                    with self.subTest(job=key):
                        condition = job.get('if', '')
                        self.assertTrue(
                            condition == self.DRAFT_RULE or condition == self.AFTER_MERGE_RULE
                            or "github.event_name == 'workflow_dispatch'" in condition,
                            f'{path.name}: job {key} would run on a draft pull request')
        self.assertGreaterEqual(seen, 3)

    def test_windows_arm64_and_intel_macs_build_after_a_merge(self):
        self.assertEqual(self.windows['jobs']['arm64']['if'], self.AFTER_MERGE_RULE)
        for key in ('runtime-regressions', 'build', 'vulkan-probe'):
            self.assertEqual(self.windows['jobs'][key]['if'], self.DRAFT_RULE, key)
        for key in ('dedicated', 'client', 'vulkan_probe'):
            with self.subTest(job=key):
                job = self.macos['jobs'][key]
                self.assertEqual(job['strategy']['matrix']['arch'], self.MACOS_ARCHS)
                # An include entry for an architecture the list leaves out would
                # bring that job back, so the runner comes from the architecture.
                self.assertNotIn('include', job['strategy']['matrix'])
                self.assertEqual(job['runs-on'],
                                 "${{ matrix.arch == 'arm64' && 'macos-26' || 'macos-15-intel' }}")
        found = re.fullmatch(
            r"\$\{\{ fromJSON\(github\.event_name == 'pull_request' && '([^']*)' \|\| '([^']*)'\) \}\}",
            self.MACOS_ARCHS)
        self.assertEqual(json.loads(found.group(1)), ['arm64'])
        self.assertEqual(json.loads(found.group(2)), ['arm64', 'x86_64'])

    def test_macos_paths_are_the_same_for_both_events(self):
        self.assertEqual(self.macos['on']['push']['paths'], self.macos['on']['pull_request']['paths'])

    def test_macos_skips_vita_only_and_run_tool_changes(self):
        paths = self.macos['on']['pull_request']['paths']
        for path in self.MACOS_SKIPS:
            with self.subTest(path=path):
                self.assertFalse(_selected(path, paths))

    def test_macos_still_runs_for_shared_and_mac_changes(self):
        paths = self.macos['on']['pull_request']['paths']
        for path in self.MACOS_RUNS:
            with self.subTest(path=path):
                self.assertTrue(_selected(path, paths))

    def test_windows_still_runs_for_shared_and_windows_changes(self):
        for path in self.WINDOWS_RUNS:
            with self.subTest(path=path):
                self.assertTrue(_selected(path, self.windows_paths))

    def test_every_script_the_windows_workflow_runs_triggers_it(self):
        # A new check added to the Windows workflow under scripts/tests must be
        # re-included in its paths, or changing only that check would not run it.
        commands = '\n'.join(step.get('run', '')
                             for job in self.windows['jobs'].values()
                             for step in job['steps'])
        referenced = sorted(set(re.findall(r'scripts/tests/[\w./-]*\w', commands)))
        self.assertIn('scripts/tests/d3dx', referenced)
        tracked = subprocess.run(['git', 'ls-files', 'scripts/tests'], cwd=ROOT, check=True,
                                 capture_output=True, text=True).stdout.split()
        for reference in referenced:
            files = [path for path in tracked
                     if path == reference or path.startswith(reference + '/')]
            with self.subTest(reference=reference):
                self.assertTrue(files)
                for path in files:
                    self.assertTrue(_selected(path, self.windows_paths), path)

    def test_no_shared_source_is_skipped(self):
        # Only the native ports' own folders are skipped under src/ (src/linux,
        # and the PS Vita's src/vita and src/gfx_gxm); every other tracked
        # source builds.
        tracked = subprocess.run(['git', 'ls-files', 'src'], cwd=ROOT, check=True,
                                 capture_output=True, text=True).stdout.split()
        skipped = [path for path in tracked
                   if not path.startswith(('src/linux/', 'src/vita/', 'src/gfx_gxm/')) and not path.endswith('.md')
                   and not _selected(path, self.windows_paths)]
        self.assertEqual(skipped, [])

    def test_the_linux_workflow_runs_when_the_windows_triggers_change(self):
        # This file checks the Windows workflow's triggers, so editing them runs it.
        for event in ('push', 'pull_request'):
            with self.subTest(event=event):
                self.assertIn('.github/workflows/build-kisarcod-win.yaml',
                              self.linux['on'][event]['paths'])

    def test_windows_builds_the_three_games_in_one_parallel_call(self):
        build = next(step for step in self.windows['jobs']['build']['steps']
                     if step.get('name', '').startswith('Build '))
        self.assertIn('--parallel', build['run'])
        self.assertIn('--target KisakCOD-sp KisakCOD-mp KisakCOD-dedi', build['run'])
        self.assertIn('if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }', build['run'])

    def test_the_pattern_reader_matches_githubs_documented_examples(self):
        # From GitHub's filter pattern cheat sheet.
        self.assertTrue(_selected('README.md', ['**/README.md']))
        self.assertTrue(_selected('server/README.md', ['**/README.md']))
        self.assertTrue(_selected('docs/a/b.md', ['docs/**']))
        self.assertFalse(_selected('docs.md', ['docs/**']))
        self.assertTrue(_selected('.github/workflows/x.yaml', ['**']))
        self.assertFalse(_selected('a/b.md', ['**', '!**/*.md']))
        self.assertTrue(_selected('scripts/tests/x.py', ['**', '!scripts/tests/**', 'scripts/tests/x.py']))
        self.assertFalse(_selected('src/linux/a.cpp', ['*.cpp']))


class VulkanWindowWorkflowTests(unittest.TestCase):
    """The Vulkan window probe: built x86_64, run on lavapipe with validation required."""

    JOB = 'vulkan_window'

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        cls.job = cls.workflow['jobs'][cls.JOB]
        cls.text = '\n'.join(step.get('run', '') for step in cls.job['steps'])

    def step(self, name):
        return next(step for step in self.job['steps'] if step.get('name') == name)

    def test_both_configurations_on_hosted_ubuntu(self):
        self.assertEqual(self.job['strategy']['matrix']['config'], ['Debug', 'RelWithDebInfo'])
        self.assertEqual(self.job['runs-on'], 'ubuntu-24.04')
        self.assertIn('timeout-minutes', self.job)

    def test_it_builds_only_the_opt_in_probe_for_x86_64(self):
        build = self.step('Build the Vulkan window probe')['run']
        self.assertIn('-DKISAK_BUILD_VULKAN_PROBE=ON', build)
        self.assertIn('-DKISAK_LINUX_ARCH=x86_64', build)
        self.assertIn('--target KisakCOD-vulkan-probe', build)
        self.assertIn("grep -Eq 'libvulkan|d3d9|dxvk'", build)
        # No game data or Steam, and of DXVK only its headers (for the D3D9
        # front): no DXVK library is built or linked, no D3D9 stand-in either.
        for forbidden in ('steam', 'zone/', 'main/', 'kisak_dxvk_native_library', 'meson setup', 'ninja -c',
                          'libd3d9'):
            self.assertNotIn(forbidden, self.text.lower())

    def test_the_sdl3_sdk_matches_the_x86_64_client(self):
        mine = self.step('Build pinned x86_64 SDL3 SDK')['run']
        client = next(step for step in self.workflow['jobs']['x64_client']['steps']
                      if step.get('name') == 'Build pinned x86_64 SDL3 SDK')['run']
        # Same cmake options, so the shared cache key holds the same SDK.
        options = lambda run: [line.strip() for line in run.splitlines() if line.strip().startswith('-D')]
        self.assertEqual(options(mine), options(client))
        self.assertEqual(self.step('Restore pinned x86_64 SDL3 SDK')['with']['key'],
                         next(step for step in self.workflow['jobs']['x64_client']['steps']
                              if step.get('name') == 'Restore pinned x86_64 SDL3 SDK')['with']['key'])
        self.assertEqual(self.job['env']['SDL3_REVISION'], self.workflow['jobs']['x64_client']['env']['SDL3_REVISION'])

    def test_it_runs_on_lavapipe_offscreen_with_validation_required(self):
        install = next(step['run'] for step in self.job['steps'] if step.get('name', '').startswith('Install '))
        for package in ('libvulkan-dev', 'mesa-vulkan-drivers', 'vulkan-validationlayers'):
            self.assertIn(package, install)
        run = self.step('Run the Vulkan window probe on lavapipe')['run']
        self.assertIn('scripts/tests/test_vulkan_window_probe.py', run)
        self.assertIn('--video-driver offscreen', run)
        self.assertIn('--validation require', run)
        self.assertNotIn('xvfb', self.text.lower())

    def test_the_generated_spirv_header_is_checked(self):
        install = next(step['run'] for step in self.job['steps'] if step.get('name', '').startswith('Install '))
        self.assertIn('spirv-tools', install)
        check = self.step("Check the probe's generated SPIR-V header")['run']
        self.assertIn('scripts/tools/vulkan/gen_spirv_header.py --check', check)

    def test_the_legacy_shader_translator_is_built_and_checked(self):
        build = self.step('Build the Vulkan window probe')['run']
        self.assertIn('-DKISAK_BUILD_VULKAN_SHADERS=ON', build)
        self.assertIn('KisakCOD-vulkan-shaders', build)
        check = self.step('Check the Legacy shader translator')['run']
        self.assertIn('scripts/tests/test_vulkan_legacy_shaders.py', check)
        self.assertIn('"$OUT_DIR/KisakCOD-vulkan-shaders"', check)
        # spirv-val validates every translated shader here.
        self.assertNotIn('--no-spirv-val', check)

    def test_the_legacy_adapter_is_built_and_checked_on_lavapipe(self):
        build = self.step('Build the Vulkan window probe')['run']
        self.assertIn('-DKISAK_BUILD_VULKAN_LEGACY=ON', build)
        self.assertIn('KisakCOD-vulkan-legacy-check', build)
        # The check's shader header is rebuilt from its assembly and compared.
        header = self.step("Check the probe's generated SPIR-V header")['run']
        self.assertIn('scripts/tools/vulkan/gen_legacy_check_shaders.py --check', header)
        run = self.step('Run the Legacy adapter check on lavapipe')['run']
        self.assertIn('scripts/tests/test_vulkan_legacy_adapter.py', run)
        self.assertIn('"$OUT_DIR/KisakCOD-vulkan-legacy-check"', run)
        self.assertIn('--video-driver offscreen', run)
        self.assertIn('--validation require', run)

    def test_the_d3d9_front_is_built_against_staged_headers_and_checked(self):
        fetch = self.step('Fetch pinned DXVK Native 2.7.1 headers')['run']
        self.assertIn('--stage-dxvk-headers "$RUNNER_TEMP/dxvk-include/dxvk"', fetch)
        # The same pin as the x86_64 client, whose renderer the front will serve.
        client = self.workflow['jobs']['x64_client']['env']
        for key in ('DXVK_REVISION', 'DXVK_DIRECTX_HEADERS_REVISION'):
            self.assertEqual(self.job['env'][key], client[key])
        build = self.step('Build the Vulkan window probe')['run']
        self.assertIn('-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON', build)
        self.assertIn('-DKISAK_DXVK_NATIVE_INCLUDE_DIR="$RUNNER_TEMP/dxvk-include/dxvk"', build)
        self.assertIn('KisakCOD-vulkan-d3d9-check', build)
        run = self.step('Run the D3D9 front check on lavapipe')['run']
        self.assertIn('scripts/tests/test_vulkan_d3d9_front.py', run)
        self.assertIn('"$OUT_DIR/KisakCOD-vulkan-d3d9-check"', run)
        self.assertIn('--video-driver offscreen', run)
        self.assertIn('--validation require', run)

    def test_the_gpu_feed_is_checked_off_and_at_its_extreme(self):
        step = self.step('Run the checks again with the GPU feed off and at its extreme')
        self.assertEqual(step['if'], "${{ matrix.config == 'Debug' }}")
        run = step['run']
        # Off: the path tile-based GPUs and MoltenVK take by default.
        self.assertIn('KISAK_VK_GPU_FEED=0 python3 -u scripts/tests/test_vulkan_d3d9_front.py', run)
        # A submission, a command buffer and a pass for every draw, in both checks.
        self.assertIn('KISAK_VK_GPU_FEED=always python3 -u scripts/tests/test_vulkan_d3d9_front.py', run)
        self.assertIn('KISAK_VK_GPU_FEED=always python3 -u scripts/tests/test_vulkan_legacy_adapter.py', run)
        self.assertEqual(run.count('--validation require'), 3)

    def test_the_checks_run_again_without_occlusion_counts(self):
        step = self.step('Run the checks again without occlusion counts')
        self.assertEqual(step['if'], "${{ matrix.config == 'Debug' }}")
        run = step['run']
        # The path of a device that cannot count samples, in both checks.
        self.assertIn('KISAK_VK_OCCLUSION_QUERIES=0 python3 -u scripts/tests/test_vulkan_d3d9_front.py', run)
        self.assertIn('KISAK_VK_OCCLUSION_QUERIES=0 python3 -u scripts/tests/test_vulkan_legacy_adapter.py', run)
        self.assertEqual(run.count('--validation require'), 2)

    def test_the_front_lock_is_checked_with_thread_sanitizer(self):
        step = self.step("Check the D3D9 front's lock with ThreadSanitizer")
        self.assertEqual(step['if'], "${{ matrix.config == 'Debug' }}")
        self.assertEqual(step['env']['SDL_VIDEODRIVER'], 'offscreen')
        self.assertIn('halt_on_error=1', step['env']['TSAN_OPTIONS'])
        # Lavapipe is not instrumented: its rasterizer threads' hand-over to
        # its queue thread reads as a race, so they are off for these runs,
        # and reports from inside the driver are dropped. The file holds
        # that one rule and nothing that could hide a race in our code.
        self.assertEqual(step['env']['LP_NUM_THREADS'], '0')
        suppressions = 'scripts/tests/vulkan_tsan_suppressions.txt'
        self.assertIn('suppressions=${{ github.workspace }}/' + suppressions, step['env']['TSAN_OPTIONS'])
        rules = [line.strip() for line in (ROOT / suppressions).read_text().splitlines()
                 if line.strip() and not line.startswith('#')]
        self.assertEqual(rules, ['race:libvulkan_lvp.so'])
        run = step['run']
        self.assertIn('-DCMAKE_CXX_FLAGS=-fsanitize=thread', run)
        self.assertIn('--scene threads', run)
        self.assertIn('--scene frame_loop', run)
        # The frame loop again with a submission after every draw, a frame
        # submitted in parts, and the lock under a 20 microsecond poller.
        self.assertIn('KISAK_VK_GPU_FEED=always "$OUT_DIR/tsan/KisakCOD-vulkan-d3d9-check" --scene frame_loop', run)
        self.assertIn('--scene gpu_feed', run)
        self.assertIn('--scene bench', run)
        # Queries reset and read on one thread, begun and ended on another.
        self.assertIn('--scene occlusion', run)
        # The feed setting changed and the statistics asked for by a second
        # thread while the first presents.
        self.assertIn('--scene tuning', run)
        self.assertEqual(run.count("grep -qx 'd3d9_check=passed'"), 7)
        self.assertIn('KISAK_VK_RECORD_THREAD=1 "$OUT_DIR/tsan/KisakCOD-vulkan-probe"', run)
        self.assertIn("grep -qx 'vulkan_probe=passed'", run)
        self.assertIn('KISAK_VK_RECORD_THREAD=1 "$OUT_DIR/tsan/KisakCOD-vulkan-legacy-check"', run)
        self.assertIn("grep -qx 'legacy_check=passed'", run)
        self.assertIn('python3 scripts/tests/test_compare_frames.py', self.step('Check the frame comparison script')['run'])

    def test_no_other_job_builds_the_probe(self):
        for name, job in self.workflow['jobs'].items():
            if name in (self.JOB, VulkanEngineWorkflowTests.JOB):
                continue
            text = '\n'.join(step.get('run', '') for step in job['steps'])
            with self.subTest(job=name):
                self.assertNotIn('KISAK_BUILD_VULKAN_PROBE', text)


class VulkanEngineWorkflowTests(unittest.TestCase):
    """The engine's programs on the Vulkan back end: i386, lavapipe, stand-ins, no game data."""

    JOB = 'vulkan_engine'
    BUILD_STEP = "Build the engine's programs with the Vulkan back end"
    RUN_STEP = 'Run the device probe through the Vulkan back end on lavapipe'

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        cls.job = cls.workflow['jobs'][cls.JOB]
        cls.text = '\n'.join(step.get('run', '') for step in cls.job['steps'])

    def step(self, name):
        return next(step for step in self.job['steps'] if step.get('name') == name)

    def test_pins_and_sdk_match_the_device_probe(self):
        probe = self.workflow['jobs']['device_probe']
        for key in ('DXVK_REVISION', 'DXVK_DIRECTX_HEADERS_REVISION', 'SDL3_REVISION'):
            self.assertEqual(self.job['env'][key], probe['env'][key])
        options = lambda run: [line.strip().rstrip('\\').strip() for line in run.splitlines()
                               if line.strip().startswith('-D')]
        mine = self.step('Build pinned 32-bit SDL3 SDK')['run']
        theirs = next(step for step in probe['steps'] if step.get('name') == 'Build pinned 32-bit SDL3 SDK')['run']
        self.assertEqual(options(mine), options(theirs))
        self.assertEqual(self.step('Restore pinned 32-bit SDL3 SDK')['with']['key'],
                         next(step for step in probe['steps']
                              if step.get('name') == 'Restore pinned 32-bit SDL3 SDK')['with']['key'])

    def test_it_builds_the_programs_with_the_back_end_against_the_stand_ins(self):
        build = self.step(self.BUILD_STEP)['run']
        for flag in ('-DKISAK_BUILD_VULKAN_BACKEND=ON', '-DKISAK_BUILD_VULKAN_LEGACY=ON', '-DKISAK_BUILD_DEVICE_PROBE=ON',
                     '-DKISAK_BUILD_WORLD_DRAW_PROBE=ON', '-DKISAK_BUILD_LINUX_MP_CLIENT=ON',
                     '-DKISAK_DXVK_NATIVE_LIBRARY="$stubs/libd3d9_unavailable.so"',
                     '-DKISAK_OPENAL_LIBRARY="$stubs/libopenal_unavailable.so"'):
            self.assertIn(flag, build)
        self.assertNotIn('KISAK_LINUX_ARCH', build)
        self.assertIn('tests/linux_device_probe/d3d9_unavailable.cpp', build)
        self.assertIn('Machine:[[:space:]]+Intel 80386', build)
        self.assertIn("grep -q 'libvulkan'", build)
        for program in ('KisakCOD-device-probe', 'KisakCOD-world-draw-probe', 'KisakCOD-mp'):
            self.assertIn(program, build)

    def test_the_programs_with_a_console_get_the_statistics_command(self):
        # r_vkStats is registered by src/gfx_d3d/r_cmds.cpp, which is not in
        # the objects that make D3D9 calls. The first build with the command
        # gave the back end's definition to those objects only, and the game
        # had no r_vkStats (found by a laptop run, not by CI).
        module = (ROOT / 'scripts/vulkan_probe/vulkan_backend.cmake').read_text()
        for pair in ('KisakCOD-world-draw-probe KisakCOD-world-draw-standin', 'KisakCOD-mp KisakCOD-mp-standin',
                     'KisakCOD-sp KisakCOD-sp-standin'):
            self.assertIn(pair, module)
        self.assertIn('target_compile_definitions(${command_objects} PRIVATE KISAK_VULKAN_BACKEND)', module)
        self.assertIn('message(FATAL_ERROR\n                "${command_objects} does not compile src/gfx_d3d/r_cmds.cpp',
                      module)
        commands = (ROOT / 'src/gfx_d3d/r_cmds.cpp').read_text()
        self.assertIn('#if defined(KISAK_VULKAN_BACKEND)\n\tCmd_AddCommandInternal("r_vkStats"', commands)
        self.assertIn('"r_vkStats: there is no Vulkan device', commands)
        build = self.step(self.BUILD_STEP)['run']
        self.assertIn('for program in KisakCOD-world-draw-probe KisakCOD-mp KisakCOD-sp; do\n'
                      'grep -qx -- "-- Vulkan back end: r_vkStats registered in $program" "$RUNNER_TEMP/configure.log"',
                      '\n'.join(line.strip() for line in build.splitlines()))
        self.assertIn('if ! grep -aq \'r_vkStats: there is no Vulkan device\' "$OUT_DIR/$program"; then', build)
        self.assertLess(build.index('cmake --build "$BUILD_DIR"'), build.index("grep -aq 'r_vkStats"))

    def test_the_device_probe_runs_through_the_back_end_on_lavapipe(self):
        install = next(step['run'] for step in self.job['steps'] if step.get('name', '').startswith('Install '))
        for package in ('libvulkan-dev', 'libvulkan1:i386', 'mesa-vulkan-drivers:i386'):
            self.assertIn(package, install)
        step = self.step(self.RUN_STEP)
        self.assertEqual(step['env']['SDL_VIDEODRIVER'], 'offscreen')
        run = step['run']
        self.assertIn('--backend vulkan', run)
        self.assertIn("grep -q '^vulkan presents=3 skipped=0 unsupported_calls=0 '", run)
        self.assertIn("grep -q '^vulkan_teardown destroyed=yes '", run)
        self.assertIn("grep -qx 'device_probe=passed renderer=shader_model_3 frames=3'", run)
        self.assertIn("grep -q '^device_probe=failed stage=direct3d '", run)
        front = self.step('Run the D3D9 front check on i386 lavapipe')['run']
        self.assertIn('scripts/tests/test_vulkan_d3d9_front.py', front)

    def test_no_dxvk_build_no_display_server_and_no_game_data(self):
        for forbidden in ('meson setup', 'ninja -c', 'libdxvk', 'xvfb', 'steam', 'zone/', 'main/', 'fs_basepath',
                          'kisak_world_draw_map', 'kisak_free_camera_map'):
            self.assertNotIn(forbidden, self.text.lower())


class CiCacheTests(unittest.TestCase):
    """The build caches only skip unchanged compiles; every check still runs."""

    # Jobs that configure and build the engine with CMake.
    CCACHE_JOBS = ('window', 'native', 'native_d3d9_boundary', 'device_probe', 'x64_dedicated',
                   'arm64_dedicated', 'armv7_dedicated', 'vita_dedicated', 'vita_mp_client', 'x64_client', 'arm64_client',
                   'vulkan_window', 'vulkan_engine')
    SDL3_JOBS = ('window', 'device_probe', 'vulkan_engine')

    @classmethod
    def setUpClass(cls):
        # The Linux workflow's jobs and the PS Vita workflow's, which were
        # part of it and keep its cache rules.
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)
        vita = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-vita.yaml').read_text(),
            Loader=yaml.BaseLoader)
        assert not set(vita['jobs']) & set(cls.workflow['jobs'])
        cls.workflow['jobs'].update(vita['jobs'])

    def step(self, job, name):
        return next(step for step in self.workflow['jobs'][job]['steps'] if step.get('name') == name)

    def test_each_cmake_job_restores_and_saves_its_own_compiler_cache(self):
        for name in self.CCACHE_JOBS:
            job = self.workflow['jobs'][name]
            names = [step.get('name') for step in job['steps']]
            with self.subTest(job=name):
                install = next(step['run'] for step in job['steps']
                               if step.get('name', '').startswith('Install '))
                self.assertIn(' ccache', install)
                restore = self.step(name, 'Restore compiler cache')
                use = self.step(name, 'Use compiler cache')
                save = self.step(name, 'Save compiler cache')
                self.assertTrue(restore['uses'].startswith('actions/cache/restore@'))
                self.assertTrue(save['uses'].startswith('actions/cache/save@'))
                self.assertEqual(restore['with']['path'], '${{ runner.temp }}/ccache')
                self.assertEqual(save['with']['path'], restore['with']['path'])
                self.assertEqual(save['with']['key'], restore['with']['key'])
                # Each run saves under its own key; the newest entry for this job
                # and configuration (this branch first, then master) is restored.
                self.assertTrue(restore['with']['key'].endswith('-${{ github.run_id }}-${{ github.run_attempt }}'))
                self.assertTrue(restore['with']['key'].startswith(restore['with']['restore-keys']))
                if 'strategy' in job:
                    self.assertIn('${{ matrix.config }}', restore['with']['restore-keys'])
                self.assertEqual(use['id'], 'ccache')
                self.assertIn('scripts/ci/use-ccache.sh', use['run'])
                self.assertEqual(save['if'], "${{ !cancelled() && steps.ccache.outcome == 'success' }}")
                # The cache is in place before the first compile and saved last.
                first_build = min(index for index, step in enumerate(job['steps'])
                                  if 'cmake --build' in step.get('run', '')
                                  or 'test_linux_window.py' in step.get('run', ''))
                self.assertLess(names.index('Use compiler cache'), first_build)
                self.assertEqual(names[-1], 'Save compiler cache')
        window_key = self.step('window', 'Restore compiler cache')['with']['restore-keys']
        self.assertIn('sdl${{ matrix.sdl }}', window_key)
        keys = [self.step(name, 'Restore compiler cache')['with']['restore-keys'] for name in self.CCACHE_JOBS]
        self.assertEqual(len(set(keys)), len(keys))

    def test_no_other_job_uses_the_compiler_cache(self):
        for name, job in self.workflow['jobs'].items():
            if name in self.CCACHE_JOBS:
                continue
            # The apt package cache (test below) is not a compiler cache.
            text = '\n'.join(step.get('run', '') + step.get('uses', '') for step in job['steps']
                              if step.get('name') != self.APT_CACHE_STEP)
            with self.subTest(job=name):
                self.assertNotIn('ccache', text)
                self.assertNotIn('actions/cache', text)

    APT_CACHE_STEP = 'Restore downloaded apt packages'

    def test_apt_installs_go_through_the_cached_retrying_script(self):
        # A slow package mirror must not time a job out before it builds:
        # every install uses scripts/ci/apt-install.sh, which retries stalled
        # downloads and keeps the .deb files in a directory each job caches.
        for name, job in self.workflow['jobs'].items():
            runs = [step.get('run', '') for step in job['steps']]
            installs = [index for index, run in enumerate(runs) if 'scripts/ci/apt-install.sh' in run]
            with self.subTest(job=name):
                for run in runs:
                    self.assertNotIn('apt-get install', run)
                if not installs:
                    self.assertNotIn(self.APT_CACHE_STEP, [step.get('name') for step in job['steps']])
                    continue
                cache = self.step(name, self.APT_CACHE_STEP)
                names = [step.get('name') for step in job['steps']]
                self.assertLess(names.index(self.APT_CACHE_STEP), installs[0])
                self.assertTrue(cache['uses'].startswith('actions/cache@'))
                self.assertEqual(cache['with']['path'], '${{ runner.temp }}/apt-archives')
                self.assertIn('${{ github.job }}', cache['with']['key'])
                # Caches are immutable: a new key must save updated .deb files,
                # while the prefix can restore the previous run's packages.
                self.assertIn('${{ github.run_id }}', cache['with']['key'])
                self.assertIn('${{ github.run_attempt }}', cache['with']['key'])
                self.assertEqual(cache['with']['restore-keys'],
                                 'apt-${{ github.job }}-${{ runner.arch }}-v1')
                self.assertGreater(int(job['timeout-minutes']),
                                   max(int(job['steps'][index]['timeout-minutes']) for index in installs))
                # Only the first install refreshes the package lists.
                for index in installs[1:]:
                    self.assertIn('apt-install.sh --no-update', runs[index])
        script = (ROOT / 'scripts/ci/apt-install.sh').read_text()
        self.assertIn('Acquire::Retries', script)
        self.assertIn('Dir::Cache::Archives=$archives', script)
        self.assertIn('${RUNNER_TEMP:-/tmp}/apt-archives', script)

    def test_apt_installer_regressions_run_in_ci(self):
        self.assertIn('python3 scripts/tests/test_ci_apt_install.py',
                      self.step('native', 'Check test-runner configuration and failure handling')['run'])

    def test_ccache_setup_launches_cmake_compiles_only(self):
        script = (ROOT / 'scripts/ci/use-ccache.sh').read_text()
        self.assertIn('set -euo pipefail', script)
        for line in ('CCACHE_DIR=$cache_dir', 'CCACHE_BASEDIR=${GITHUB_WORKSPACE:?}',
                     'CMAKE_C_COMPILER_LAUNCHER=ccache', 'CMAKE_CXX_COMPILER_LAUNCHER=ccache'):
            self.assertIn(line, script)
        self.assertIn('"${GITHUB_ENV:?}"', script)
        self.assertNotIn('CXX=', script.replace('CMAKE_CXX_COMPILER_LAUNCHER=', ''))

    def test_sdl3_sdk_cache_follows_the_pin_and_this_file_and_is_still_checked(self):
        for name in self.SDL3_JOBS:
            job = self.workflow['jobs'][name]
            names = [step.get('name') for step in job['steps']]
            with self.subTest(job=name):
                cache = self.step(name, 'Restore pinned 32-bit SDL3 SDK')
                self.assertTrue(cache['uses'].startswith('actions/cache@'))
                self.assertEqual(cache['with']['path'], '${{ runner.temp }}/SDL3-sdk')
                key = cache['with']['key']
                self.assertTrue('877399b2b2cf21e67554ed9046410f268ce1d1b2' in key
                                or '${{ env.SDL3_REVISION }}' in key, key)
                self.assertIn("hashFiles('.github/workflows/build-kisakcod-linux.yaml')", key)
                self.assertLess(names.index('Restore pinned 32-bit SDL3 SDK'),
                                names.index('Build pinned 32-bit SDL3 SDK'))
                build = self.step(name, 'Build pinned 32-bit SDL3 SDK')['run']
                self.assertIn('if [[ ! -f "$RUNNER_TEMP/SDL3-sdk/lib/libSDL3.so.0" ]]; then', build)
                # The ELF check runs on a restored SDK too.
                tail = build[build.index('\nfi\n'):]
                self.assertIn('readelf -h "$RUNNER_TEMP/SDL3-sdk/lib/libSDL3.so.0"', tail)
                self.assertIn('grep -q ELF32', tail)
        self.assertEqual(self.step('window', 'Restore pinned 32-bit SDL3 SDK')['if'], 'matrix.sdl == 3')

    def test_engine_builds_use_every_runner_core(self):
        text = '\n'.join(step.get('run', '') for name in self.CCACHE_JOBS
                         for step in self.workflow['jobs'][name]['steps'])
        self.assertNotIn('--parallel 2', text)
        self.assertNotIn('--jobs 2', text)
        self.assertIn('--jobs "$(nproc)"', text)


if __name__ == '__main__':
    unittest.main()
