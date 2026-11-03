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
    # x64_client does it to build the x86_64 multiplayer client against stand-ins.
    BOUNDARY_JOBS = ('native_d3d9_boundary', 'device_probe', 'x64_client')

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
        self.assertEqual(script.count('add_argument('), 1)
        self.assertIn('add_argument("--binary"', script)
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

    CONCURRENCY = {
        'group': "${{ github.workflow }}-${{ github.event_name == 'workflow_dispatch' && github.run_id || github.ref }}",
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
    )
    # Changes that must still build Windows.
    WINDOWS_RUNS = (
        'CMakeLists.txt',
        'src/gfx_d3d/r_image.cpp',
        'src/universal/q_shared.h',
        'src/win32/win_main.cpp',
        'scripts/common_files.cmake',
        'scripts/platform/win32/platform.cmake',
        'scripts/ci/check-windows-bootstrap.ps1',
        'scripts/tests/d3dx/CMakeLists.txt',
        'scripts/tests/test_voice_lifecycle.py',
        'deps/some_library/source.c',
        '.github/workflows/build-kisarcod-win.yaml',
    )

    @classmethod
    def setUpClass(cls):
        def load(name):
            return yaml.load((ROOT / '.github/workflows' / name).read_text(),
                             Loader=yaml.BaseLoader)
        cls.linux = load('build-kisakcod-linux.yaml')
        cls.windows = load('build-kisarcod-win.yaml')
        cls.windows_paths = cls.windows['on']['pull_request']['paths']

    def test_a_newer_push_cancels_only_the_same_branchs_older_run(self):
        for name, workflow in (('linux', self.linux), ('windows', self.windows)):
            with self.subTest(workflow=name):
                self.assertEqual(workflow['concurrency'], self.CONCURRENCY)

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
        # Only src/linux is skipped under src/; every other tracked source builds.
        tracked = subprocess.run(['git', 'ls-files', 'src'], cwd=ROOT, check=True,
                                 capture_output=True, text=True).stdout.split()
        skipped = [path for path in tracked
                   if not path.startswith('src/linux/') and not path.endswith('.md')
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


class CiCacheTests(unittest.TestCase):
    """The build caches only skip unchanged compiles; every check still runs."""

    # Jobs that configure and build the engine with CMake.
    CCACHE_JOBS = ('window', 'native', 'native_d3d9_boundary', 'device_probe', 'x64_dedicated',
                   'x64_client')
    SDL3_JOBS = ('window', 'device_probe')

    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build-kisakcod-linux.yaml').read_text(),
            Loader=yaml.BaseLoader)

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
            text = '\n'.join(step.get('run', '') + step.get('uses', '') for step in job['steps'])
            with self.subTest(job=name):
                self.assertNotIn('ccache', text)
                self.assertNotIn('actions/cache', text)

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
