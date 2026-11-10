#!/usr/bin/env python3
"""Runs KisakCOD-vulkan-d3d9-check (src/gfx_vk/tools/vk_d3d9_check.cpp) and checks it.

Needs a built check, SDL3 and a Vulkan driver. In CI that is Mesa's lavapipe
behind SDL's offscreen video driver, so no GPU, display server or game data is
involved. The checks:

  usage          bad arguments exit with status 2 and print no result
  unknown-scene  --scene with a name no scene has fails at the "usage" stage
  one-scene      --scene runs that scene alone and passes, with no unsupported
                 call
  present-mode   the device asks for no vsync, as the engine does by default:
                 with KISAK_VK_PRESENT_MODE=fifo its swapchain is FIFO all the
                 same (the one mode every surface offers), and without the
                 switch lavapipe's surface gives it a mode that does not wait
                 for the display
  bench          --scene bench, which a full run leaves out, passes and prints
                 its timings (they depend on the machine and are not judged)
  bench-runs     --scene bench_runs, which a full run leaves out too: frames of
                 768 draws in runs of eight that share their state, as the
                 engine's sorted draws do. It runs with the validation layer,
                 where the adapter checks every block, pipeline and dynamic
                 state it keeps from an earlier draw, and its last frame is
                 read back; timings are printed and not judged
  full           a full run passes: every scene in SCENES ran, had points and
                 no mismatch; the only unsupported call is the one the states
                 scene makes on purpose; frames were presented; no object
                 outlived its release; the device and IDirect3D9 were destroyed
                 at their last release; the validation layer reported no errors;
                 the swap fence flushed the next frame once; and the frame of
                 600 draws was submitted in as many parts as the device's GPU
                 feed setting allows (the setting is read from the device's
                 own log line, so KISAK_VK_GPU_FEED and a GPU that has it off
                 by default are both covered); and the occlusion scene counted
                 samples, or with KISAK_VK_OCCLUSION_QUERIES=0 did not, as the
                 device's log line says (its exact counts are the scene's own
                 points); and the tuning scene changed the GPU feed while the
                 device ran, drew its five frames of 600 draws under five
                 settings, logged every change and ended on the device's own
                 setting (what each setting did to its frame, and that the
                 statistics count from where they were last started, are the
                 scene's own points)

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
SCENES = ('adapter', 'states', 'draw', 'render_target', 'shadow_map', 'references', 'threads', 'queries', 'occlusion',
          'frame_loop', 'gpu_feed', 'tuning', 'reset', 'present')


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

    # present-mode
    fifo = dict(env)
    fifo['KISAK_VK_PRESENT_MODE'] = 'fifo'
    code, stdout, _ = run(binary, ['--scene', 'draw', '--validation', args.validation], fifo)
    found, _ = parse(stdout)
    mode = re.search(r'^d3d9_check\.log=swapchain \S+ \S+ (\w+), \d+ images', stdout, re.M)
    checks.expect('KISAK_VK_PRESENT_MODE=fifo run passes', code == 0 and found.get('d3d9_check') == 'passed',
                  f'status {code}\n{stdout}')
    checks.expect('KISAK_VK_PRESENT_MODE=fifo gives a FIFO swapchain', bool(mode) and mode.group(1) == 'FIFO',
                  mode.group(0) if mode else 'no swapchain line')

    # bench (timings: without the validation layer)
    code, stdout, _ = run(binary, ['--scene', 'bench', '--validation', 'off'], env)
    found, scenes = parse(stdout)
    checks.expect('bench passes', code == 0 and found.get('d3d9_check') == 'passed', f'status {code}\n{stdout}')
    checks.expect('bench ran alone', [s[0] for s in scenes] == ['bench'], repr(scenes))
    bench = found.get('d3d9_check.bench', '')
    checks.expect('bench prints its timings',
                  re.match(r'frames=\d+ draws=\d+ calls=\d+ draw_ms_average=[\d.]+ draw_ms_min=[\d.]+ '
                           r'polls_per_frame=\d+ submissions_per_frame=[\d.]+$', bench) is not None,
                  bench or 'missing')
    if bench:
        print(f'bench: {bench}')

    # bench_runs (with the validation layer: the adapter then checks what it keeps between draws)
    code, stdout, _ = run(binary, ['--scene', 'bench_runs', '--validation', args.validation], env, timeout=900)
    found, scenes = parse(stdout)
    checks.expect('bench_runs passes', code == 0 and found.get('d3d9_check') == 'passed', f'status {code}\n{stdout}')
    checks.expect('bench_runs ran alone and its frame matches',
                  len(scenes) == 1 and scenes[0][0] == 'bench_runs' and scenes[0][1] > 0 and scenes[0][2] == 0,
                  repr(scenes))
    bench_runs = found.get('d3d9_check.bench_runs', '')
    checks.expect('bench_runs prints its timings',
                  re.match(r'frames=\d+ draws=\d+ calls=\d+ draw_ms_average=[\d.]+ draw_ms_min=[\d.]+ '
                           r'polls_per_frame=\d+ submissions_per_frame=[\d.]+$', bench_runs) is not None,
                  bench_runs or 'missing')
    if bench_runs:
        print(f'bench_runs: {bench_runs}')

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
    # No vsync was asked for: IMMEDIATE where the surface has it, else
    # MAILBOX, and FIFO only where it has neither. CI's software GPU has one.
    mode = re.search(r'^d3d9_check\.log=swapchain \S+ \S+ (\w+), \d+ images', stdout, re.M)
    checks.expect('the swapchain logs its present mode', mode is not None)
    if mode and env.get('KISAK_VK_PRESENT_MODE') is None and re.search(r'vulkan d3d9: device llvmpipe ', stdout):
        checks.expect('without vsync lavapipe presents without waiting', mode.group(1) in ('IMMEDIATE', 'MAILBOX'),
                      mode.group(1))
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
    # A swap fence polled with FLUSH while its frame is unfinished submits the
    # next frame's work ahead of its Present, once (not at all with
    # KISAK_VK_FENCE_FLUSH=0). Without the record thread a software GPU has
    # finished the frame inside vkQueueSubmit2, before any poll.
    fence_flushes = '0' if env.get('KISAK_VK_FENCE_FLUSH') == '0' else '1'
    fence_paths = [f'ahead flushes={fence_flushes}']
    if env.get('KISAK_VK_RECORD_THREAD') == '0':
        fence_paths.append('finished_first flushes=0')
    # The GPU feed, as the device says it is set: "off", "every N draws", or
    # "after every draw" (KISAK_VK_GPU_FEED=always, for testing).
    feed = re.search(r'legacy: device ready: .*, GPU feed (off|after every draw|every (\d+) draws)$', stdout, re.M)
    checks.expect('the device logs its GPU feed setting', feed is not None)
    feed_setting = feed.group(1) if feed else 'off'
    if feed_setting == 'after every draw':
        # Nothing is left to flush at the poll: every draw was submitted as it was made.
        fence_paths = ['ahead flushes=0', 'finished_first flushes=0']
    checks.expect('swap fence poll flushes the next frame once',
                  found.get('d3d9_check.fence_flush') in fence_paths,
                  found.get('d3d9_check.fence_flush', 'missing'))
    # The frame of 600 draws: whole with the feed off; a part for every draw
    # with "after every draw"; else at least one part (the feed waits for at
    # most four times its draw count, however far behind the GPU is) and at
    # most one for every N draws.
    parts = re.match(r'draws=(\d+) parts=(\d+)$', found.get('d3d9_check.gpu_feed', ''))
    checks.expect('the feed scene reports its parts', parts is not None, found.get('d3d9_check.gpu_feed', 'missing'))
    if parts:
        draws, count = int(parts.group(1)), int(parts.group(2))
        if feed_setting == 'off':
            low, high = 0, 0
        elif feed_setting == 'after every draw':
            low, high = draws, draws
        else:
            every = int(feed.group(2))
            low, high = (1 if draws >= 4 * every else 0), draws // every
        checks.expect(f'GPU feed {feed_setting}: the frame went to the GPU in {low} to {high} parts',
                      low <= count <= high, f'{count} parts')
    # Occlusion queries, as the device says it has them: counted, unless the
    # GPU cannot or KISAK_VK_OCCLUSION_QUERIES=0 says not to. The scene's 700
    # queries then took at least one Vulkan query each, out of two pools (it
    # holds 300 at once, and a pool has 256).
    counted = re.search(r'legacy: device ready: .*, occlusion queries (counted|not counted), ', stdout)
    checks.expect('the device logs whether it counts occlusion queries', counted is not None)
    if counted and env.get('KISAK_VK_OCCLUSION_QUERIES') == '0':
        checks.expect('KISAK_VK_OCCLUSION_QUERIES=0 turns counting off', counted.group(1) == 'not counted',
                      counted.group(1))
    elif counted and re.search(r'vulkan d3d9: device llvmpipe ', stdout):
        # CI's software GPU can count, so a run there that does not is a fault.
        checks.expect('lavapipe counts occlusion queries', counted.group(1) == 'counted', counted.group(1))
    occlusion = re.match(r'counted=(yes|no) queries=(\d+) counts=(\d+) pools=(\d+) results=(\d+) empty=(\d+) '
                         r'samples=(\d+)$', found.get('d3d9_check.occlusion', ''))
    checks.expect('the occlusion scene reports its counts', occlusion is not None,
                  found.get('d3d9_check.occlusion', 'missing'))
    if counted and occlusion:
        queries, counts, pools, results, empty, samples = (int(occlusion.group(i)) for i in range(2, 8))
        if counted.group(1) == 'counted':
            checks.expect('occlusion queries were counted',
                          occlusion.group(1) == 'yes' and counts >= queries and pools == 2,
                          found['d3d9_check.occlusion'])
            # The statistics r_vkStats prints for the sun: every query the
            # scene read is a result, a few of them had no sample, and the
            # 700 alone drew more than 64 samples each on average.
            checks.expect('the occlusion statistics saw the results',
                          results >= queries and 1 <= empty < results and samples >= 64 * queries,
                          found['d3d9_check.occlusion'])
            line = re.search(r'vulkan d3d9: occlusion queries: counted, with \d+ Vulkan queries from \d+ pools; '
                             r'(\d+) results, (\d+) of them with no sample, [\d.]+ samples on average, the last \d+$',
                             stdout, re.M)
            checks.expect('the occlusion log line gives the results', line is not None and int(line.group(1)) >= results,
                          line.group(0) if line else 'no such line')
        else:
            checks.expect('occlusion queries were not counted',
                          occlusion.group(1) == 'no' and counts == 0 and pools == 0 and results == 0 and samples == 0,
                          found['d3d9_check.occlusion'])
    # The settings changed while the device ran (the engine's r_vkGpuFeed
    # dvars): five frames of 600 draws under settings of the scene's own, one
    # of them a part for every draw and one the rule that sends up to five
    # parts early, then back to what the device began with, which is the last
    # change it logged. Six presents took the settings up.
    tuning = re.match(r'frames=(\d+) draws=(\d+) parts=(\d+) early=(\d+) idle_frame_parts=(\d+) '
                      r'idle_frame_early=(\d+) changes=(\d+) gpu_time=(yes|no)$', found.get('d3d9_check.tuning', ''))
    checks.expect('the tuning scene reports its frames', tuning is not None, found.get('d3d9_check.tuning', 'missing'))
    if tuning:
        frames, draws, parts, early, idle_parts, idle_early, changes = (int(tuning.group(i)) for i in range(1, 8))
        checks.expect('the tuning scene drew five frames of 600 draws', (frames, draws) == (6, 3000),
                      found['d3d9_check.tuning'])
        checks.expect('a part for every draw of one frame, and more', parts >= 600 + idle_parts,
                      found['d3d9_check.tuning'])
        checks.expect('the idle rule sent one to five parts early', 1 <= idle_early <= 5 and early == idle_early
                      and idle_early <= idle_parts <= idle_early + 2, found['d3d9_check.tuning'])
        # A device that begins with the feed off logs no change to "off".
        checks.expect('every change of the setting was logged', changes == (5 if feed_setting == 'off' else 6),
                      found['d3d9_check.tuning'])
    changed = re.findall(r'^d3d9_check\.log=legacy: GPU feed now (.+)$', stdout, re.M)
    checks.expect("the device ended on its own GPU feed setting", bool(changed) and changed[-1] == feed_setting,
                  changed[-1] if changed else 'no change logged')
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
