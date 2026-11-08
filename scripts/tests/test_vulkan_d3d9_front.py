#!/usr/bin/env python3
"""Runs KisakCOD-vulkan-d3d9-check (src/gfx_vk/tools/vk_d3d9_check.cpp) and checks it.

Needs a built check, SDL3 and a Vulkan driver. In CI that is Mesa's lavapipe
behind SDL's offscreen video driver, so no GPU, display server or game data is
involved. The checks:

  usage          bad arguments exit with status 2 and print no result
  unknown-scene  --scene with a name no scene has fails at the "usage" stage
  one-scene      --scene runs that scene alone and passes, with no unsupported
                 call
  full           a full run passes: every scene in SCENES ran, had points and
                 no mismatch; the only unsupported call is the one the states
                 scene makes on purpose; frames were presented; no object
                 outlived its release; the device and IDirect3D9 were destroyed
                 at their last release; the validation layer reported no errors

Usage:
  test_vulkan_d3d9_front.py --binary build/bin/KisakCOD-vulkan-d3d9-check
      [--video-driver offscreen] [--validation require|auto|off]
"""

import argparse
import os
import re
import subprocess
import sys

# The scenes the check runs, in order. A scene added to the check is added here.
SCENES = ('adapter', 'states', 'draw', 'render_target', 'shadow_map', 'references', 'threads', 'queries', 'reset', 'present')


def run(binary, args, env, timeout=300):
    result = subprocess.run([binary] + args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=timeout)
    return result.returncode, result.stdout, result.stderr


def parse(stdout):
    found = {}
    scenes = []
    for line in stdout.splitlines():
        if line.startswith('d3d9_check.log='):
            continue
        scene = re.match(r'd3d9_check\.scene=(\w+) points=(\d+) mismatched=(\d+)', line)
        if scene:
            scenes.append((scene.group(1), int(scene.group(2)), int(scene.group(3))))
            continue
        if '=' in line:
            key, _, value = line.partition('=')
            found[key] = value
    return found, scenes


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
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    env = dict(os.environ)
    env['SDL_VIDEODRIVER'] = args.video_driver
    env.pop('VK_INSTANCE_LAYERS', None)
    checks = Checks()

    # usage
    for bad in (['--validation', 'maybe'], ['--device', '-1'], ['--device', 'x'], ['--scene'], ['--bogus']):
        code, stdout, _ = run(binary, bad, env)
        checks.expect(f"usage {' '.join(bad)}", code == 2 and 'd3d9_check' not in stdout, f'status {code}')

    # unknown-scene
    code, stdout, _ = run(binary, ['--scene', 'no_such_scene', '--validation', 'off'], env)
    checks.expect('unknown scene fails at the usage stage',
                  code == 1 and re.search(r'^d3d9_check=failed stage=usage$', stdout, re.M) is not None,
                  f'status {code}\n{stdout}')

    # one-scene
    code, stdout, _ = run(binary, ['--scene', 'draw', '--validation', args.validation], env)
    found, scenes = parse(stdout)
    checks.expect('one scene passes', code == 0 and found.get('d3d9_check') == 'passed', f'status {code}\n{stdout}')
    checks.expect('one scene ran alone', [s[0] for s in scenes] == ['draw'], repr(scenes))
    checks.expect('one scene made no unsupported call',
                  found.get('d3d9_check.unsupported_calls') == '0 expected=0',
                  found.get('d3d9_check.unsupported_calls', 'missing'))

    # full
    code, stdout, stderr = run(binary, ['--validation', args.validation], env)
    print(stdout, end='')
    if stderr.strip():
        print(stderr, end='', file=sys.stderr)
    found, scenes = parse(stdout)
    checks.expect('full run passes', code == 0 and found.get('d3d9_check') == 'passed', f'status {code}')
    names = [s[0] for s in scenes]
    checks.expect('every scene ran in order', tuple(names) == SCENES, f'ran {names}')
    for name, points, mismatched in scenes:
        checks.expect(f'scene {name} matches', points > 0 and mismatched == 0,
                      f'points {points} mismatched {mismatched}')
    checks.expect('only the deliberate unsupported call', found.get('d3d9_check.unsupported_calls') == '1 expected=1',
                  found.get('d3d9_check.unsupported_calls', 'missing'))
    presents = re.match(r'(\d+) skipped=(\d+)$', found.get('d3d9_check.presents', ''))
    checks.expect('frames presented', bool(presents) and int(presents.group(1)) >= len(SCENES),
                  found.get('d3d9_check.presents', 'missing'))
    checks.expect('no object outlived its release', found.get('d3d9_check.live_objects_at_teardown') == '0',
                  found.get('d3d9_check.live_objects_at_teardown', 'missing'))
    checks.expect('device destroyed at its last release',
                  found.get('d3d9_check.device_release') == '0 destroyed=yes',
                  found.get('d3d9_check.device_release', 'missing'))
    polls = found.get('d3d9_check.event_query_polls', '')
    checks.expect('event query finished', polls.isdigit(), polls or 'missing')
    if args.validation == 'require':
        checks.expect('validation was on', found.get('d3d9_check.validation') == 'on',
                      found.get('d3d9_check.validation', 'missing'))
    checks.expect('no validation errors', found.get('d3d9_check.validation_errors') == '0',
                  found.get('d3d9_check.validation_errors', 'missing'))

    if checks.failures:
        print(f'{len(checks.failures)} check(s) failed: {", ".join(checks.failures)}')
        return 1
    print('all Vulkan D3D9 front checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
