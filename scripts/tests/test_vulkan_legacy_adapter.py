#!/usr/bin/env python3
"""Runs KisakCOD-vulkan-legacy-check (src/gfx_vk/tools/vk_legacy_check.cpp) and checks it.

Needs a built check, SDL3 and a Vulkan driver. In CI that is Mesa's lavapipe
behind SDL's offscreen video driver, so no GPU, display server or game data is
involved. The checks:

  usage          bad arguments exit with status 2 and print no result
  unknown-scene  --scene with a name no scene has fails at the "usage" stage
  one-scene      --scene runs that scene alone and passes
  full           a full run passes: every scene in SCENES ran, had points and
                 no mismatched pixel, every scene was presented, the dynamic
                 buffer was renamed, the event query finished, occlusion
                 queries were counted (always on lavapipe, never with
                 KISAK_VK_OCCLUSION_QUERIES=0, else as the device can),
                 pipelines were built and the validation layer reported no
                 errors

Usage:
  test_vulkan_legacy_adapter.py --binary build/bin/KisakCOD-vulkan-legacy-check
      [--video-driver offscreen] [--validation require|auto|off]
"""

import argparse
import os
import re
import subprocess
import sys

# The scenes the check draws, in order. A scene added to the check is added here.
SCENES = (
    'clear', 'vertex_color', 'half_pixel', 'texture_levels', 'formats', 'alpha_test', 'blend', 'depth',
    'stencil', 'cull', 'render_to_texture', 'dynamic_buffer', 'static_buffers', 'cube_volume',
    'shader_model_2', 'partial_precision', 'missing_input', 'event_query', 'occlusion_query', 'state_reuse',
    'shader_cache',
)


def run(binary, args, env, timeout=300):
    result = subprocess.run([binary] + args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=timeout)
    return result.returncode, result.stdout, result.stderr


def parse(stdout):
    found = {}
    scenes = []
    for line in stdout.splitlines():
        if line.startswith('legacy_check.log='):
            continue
        scene = re.match(r'legacy_check\.scene=(\w+) points=(\d+) mismatched=(\d+)', line)
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
        checks.expect(f"usage {' '.join(bad)}", code == 2 and 'legacy_check' not in stdout, f'status {code}')

    # unknown-scene
    code, stdout, _ = run(binary, ['--scene', 'no_such_scene', '--validation', 'off'], env)
    checks.expect('unknown scene fails at the usage stage',
                  code == 1 and re.search(r'^legacy_check=failed stage=usage$', stdout, re.M) is not None,
                  f'status {code}\n{stdout}')

    # one-scene
    code, stdout, _ = run(binary, ['--scene', 'half_pixel', '--validation', args.validation], env)
    found, scenes = parse(stdout)
    checks.expect('one scene passes', code == 0 and found.get('legacy_check') == 'passed', f'status {code}\n{stdout}')
    checks.expect('one scene ran alone', [s[0] for s in scenes] == ['half_pixel'], repr(scenes))

    # full
    code, stdout, stderr = run(binary, ['--validation', args.validation], env)
    print(stdout, end='')
    if stderr.strip():
        print(stderr, end='', file=sys.stderr)
    found, scenes = parse(stdout)
    checks.expect('full run passes', code == 0 and found.get('legacy_check') == 'passed', f'status {code}')
    names = [s[0] for s in scenes]
    checks.expect('every scene ran in order', tuple(names) == SCENES, f'ran {names}')
    for name, points, mismatched in scenes:
        checks.expect(f'scene {name} matches', points > 0 and mismatched == 0,
                      f'points {points} mismatched {mismatched}')
    checks.expect('every scene presented', found.get('legacy_check.presented') == str(len(SCENES)),
                  found.get('legacy_check.presented', 'missing'))
    renames = found.get('legacy_check.dynamic_renames', '0')
    checks.expect('dynamic buffer renamed', renames.isdigit() and int(renames) > 0, renames)
    polls = found.get('legacy_check.event_query_polls', '')
    checks.expect('event query finished', polls.isdigit(), polls or 'missing')
    # The occlusion scene's exact counts are its own points; here, which path
    # it took: counting, unless the GPU cannot or the switch says not to.
    occlusion = found.get('legacy_check.occlusion_query', 'missing')
    if env.get('KISAK_VK_OCCLUSION_QUERIES') == '0':
        checks.expect('KISAK_VK_OCCLUSION_QUERIES=0 turns counting off', occlusion == 'not counted', occlusion)
    elif found.get('legacy_check.device', '').startswith('llvmpipe'):
        checks.expect('lavapipe counts occlusion queries', occlusion == 'counted', occlusion)
    else:
        checks.expect('the occlusion scene says whether it counted', occlusion in ('counted', 'not counted'),
                      occlusion)
    stats = re.search(r'pipelines (\d+) in_frame (\d+)', found.get('legacy_check.stats', ''))
    checks.expect('pipelines built', bool(stats) and int(stats.group(1)) > 0, found.get('legacy_check.stats', 'missing'))
    if args.validation == 'require':
        checks.expect('validation was on', found.get('legacy_check.validation', '').startswith('on'),
                      found.get('legacy_check.validation', 'missing'))
    checks.expect('no validation errors', found.get('legacy_check.validation_errors') == '0',
                  found.get('legacy_check.validation_errors', 'missing'))

    if checks.failures:
        print(f'{len(checks.failures)} check(s) failed: {", ".join(checks.failures)}')
        return 1
    print('all Vulkan Legacy adapter checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
