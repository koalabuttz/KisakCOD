#!/usr/bin/env python3
"""Runs KisakCOD-vulkan-probe (src/gfx_vk/tools/vk_window_probe.cpp) and checks it.

Needs a built probe, SDL3 and a Vulkan driver. In CI that is Mesa's lavapipe
(a CPU Vulkan driver) behind SDL's offscreen video driver, so no GPU, display
server or game data is involved. The checks:

  usage          bad arguments exit with status 2 and change nothing
  list           --list-devices finds at least one usable device
  no-driver      with no Vulkan driver visible the probe fails closed (status 1)
                 at the "loader" stage (SDL finds no surface extension without
                 a driver) or the "core" stage, instead of crashing
  frames         a full run (resize halfway, capture of the last frame) passes,
                 the swapchain followed the resize, the captured frame is the
                 cleared colour, the clear pass was timed, and the validation
                 layer (when loaded) reported no errors

Usage:
  test_vulkan_window_probe.py --binary build/bin/KisakCOD-vulkan-probe
      [--video-driver offscreen] [--validation require|auto|off] [--frames N]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile


def run(binary, args, env, timeout=180):
    result = subprocess.run([binary] + args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=timeout)
    return result.returncode, result.stdout, result.stderr


def values(stdout):
    out = {}
    for line in stdout.splitlines():
        if '=' in line and not line.startswith('vulkan_probe.log='):
            key, _, value = line.partition('=')
            out[key] = value
    return out


class Checks:
    def __init__(self):
        self.failures = []

    def expect(self, name, condition, detail=''):
        print(f"{'ok  ' if condition else 'FAIL'} {name}{': ' + detail if detail and not condition else ''}")
        if not condition:
            self.failures.append(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--video-driver', default='offscreen',
                        help='SDL_VIDEODRIVER for the runs (default: offscreen, which needs no display)')
    parser.add_argument('--validation', default='require', choices=('require', 'auto', 'off'))
    parser.add_argument('--frames', type=int, default=120)
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    env = dict(os.environ)
    env['SDL_VIDEODRIVER'] = args.video_driver
    env.pop('VK_INSTANCE_LAYERS', None)
    checks = Checks()

    # usage
    for bad in (['--frames', '0'], ['--size', '10x10'], ['--validation', 'maybe'], ['--bogus']):
        code, stdout, _ = run(binary, bad, env)
        checks.expect(f"usage {' '.join(bad)}", code == 2 and 'vulkan_probe' not in stdout, f'status {code}')

    # list
    code, stdout, stderr = run(binary, ['--list-devices', '--validation', 'off'], env)
    print(stdout, end='')
    checks.expect('list exits 0', code == 0, f'status {code}, stderr {stderr.strip()}')
    checks.expect('list finds a usable device', 'usable, meets the requirements' in stdout)

    # no-driver: point the loader at an empty driver directory.
    with tempfile.TemporaryDirectory() as empty:
        no_driver = dict(env)
        no_driver['VK_DRIVER_FILES'] = empty
        no_driver['VK_ICD_FILENAMES'] = empty
        code, stdout, _ = run(binary, ['--frames', '5', '--validation', 'off'], no_driver)
        stage = re.search(r'^vulkan_probe=failed stage=(\w+)$', stdout, re.M)
        checks.expect('no-driver fails closed', code == 1 and bool(stage) and stage.group(1) in ('loader', 'core'),
                      f'status {code}\n{stdout}')

    # frames
    code, stdout, stderr = run(binary, ['--frames', str(args.frames), '--validation', args.validation,
                                        '--resize', '--check-capture'], env)
    print(stdout, end='')
    if stderr.strip():
        print(stderr, end='', file=sys.stderr)
    found = values(stdout)
    checks.expect('frames run passes', code == 0 and found.get('vulkan_probe') == 'passed', f'status {code}')
    presented = found.get('vulkan_probe.frames_presented', '0')
    checks.expect('all frames presented', presented.isdigit() and int(presented) >= args.frames)
    resize = re.match(r'(\d+)x(\d+) window_pixels=(\d+)x(\d+) swapchain=(\d+)x(\d+) builds=(\d+)',
                      found.get('vulkan_probe.resize', ''))
    checks.expect('swapchain followed the resize', bool(resize) and resize.group(3, 4) == resize.group(5, 6)
                  and int(resize.group(7)) >= 2, found.get('vulkan_probe.resize', 'missing'))
    checks.expect('capture is the cleared colour',
                  found.get('vulkan_probe.capture', '').endswith('mismatched_pixels=0'),
                  found.get('vulkan_probe.capture', 'missing'))
    checks.expect('clear pass timed', found.get('vulkan_probe.clear_pass_gpu_ms', '').startswith('avg '),
                  found.get('vulkan_probe.clear_pass_gpu_ms', 'missing'))
    if args.validation == 'require':
        checks.expect('validation with synchronisation checks was on',
                      found.get('vulkan_probe.validation') == 'on+sync', found.get('vulkan_probe.validation'))
    checks.expect('no validation errors', found.get('vulkan_probe.validation_errors') == '0')

    if checks.failures:
        print(f'{len(checks.failures)} check(s) failed: {", ".join(checks.failures)}')
        return 1
    print('all Vulkan window probe checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
