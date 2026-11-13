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
                 scene's own points); and the Modern path's statistics lines
                 count the 61 anti-aliased scenes of modern_taa,
                 modern_linear, modern_bloom, modern_effects, modern_lit,
                 modern_lit_shader, modern_tonemap, modern_exposure,
                 modern_velocity, modern_normals, modern_ao and
                 modern_reflections, 20 of them without history, the 17
                 scenes of modern_linear, modern_bloom, modern_effects,
                 modern_lit, modern_lit_shader, modern_tonemap,
                 modern_exposure, modern_normals, modern_ao and
                 modern_reflections in
                 linear light with one copy read from them (none with
                 KISAK_VK_MODERN_LINEAR=0), modern_bloom's 2 scenes with
                 bloom (none with KISAK_VK_MODERN_LINEAR=0 or
                 KISAK_VK_MODERN_BLOOM=0), modern_lit's, modern_lit_shader's,
                 modern_tonemap's and modern_ao's scenes with lit surfaces
                 kept over white, inputs changed 2.8 times in each (twice in
                 modern_lit and modern_tonemap, six times in
                 modern_lit_shader, once in modern_ao), and the seven pixel
                 modules modern_lit_shader (six) and modern_ao (one: its
                 floor is depth-tested against the prepass, a depth compare
                 of its own; six in all with KISAK_VK_DEPTH_COMPARE=dynamic,
                 where the compare is not built in) built as our own lit
                 shaders (none
                 with KISAK_VK_MODERN_LINEAR=0 or KISAK_VK_MODERN_LIT=0, and
                 no lit shaders of our own with KISAK_VK_MODERN_SHADERS=0),
                 modern_velocity's 14 scenes with a surface that moves on its
                 own (14 surfaces), and its 8 scenes that kept a skinned
                 surface's vertices, 7 of them drawn from in the scene after
  linear-off     --scene modern_linear passes with KISAK_VK_MODERN_LINEAR=0
                 too, where the scene is drawn as always (the scene's own
                 points tell the two apart)
  bloom-off      --scene modern_bloom passes with KISAK_VK_MODERN_BLOOM=0 and
                 with KISAK_VK_MODERN_LINEAR=0, where the glow is left to the
                 renderer and no bloom is drawn
  effects-linear --scene modern_effects passes with
                 KISAK_VK_MODERN_EFFECTS=linear, where effects add as light in
                 the scene instead of on the display image
  tonemap        --scene modern_tonemap passes with each
                 KISAK_VK_MODERN_TONEMAP curve (clamp, agx, aces, and an
                 unknown name, which logs that and keeps the default,
                 neutral), and the
                 device logs the curve it uses
  exposure       --scene modern_exposure passes with KISAK_VK_MODERN_EXPOSURE=1
                 (the other runs set it to 0 unless the caller set it, since
                 auto exposure moves their colours), with its 5 scenes
                 measured, 4 of them cuts
  lit-off        --scene modern_lit passes with KISAK_VK_MODERN_LIT=0 and
                 with KISAK_VK_MODERN_LINEAR=0, where lit surfaces store
                 their colour as a display value and no lit line is logged
  shaders-off    --scene modern_lit_shader passes with
                 KISAK_VK_MODERN_SHADERS=0, where the game's pixel shader is
                 drawn as it is and no line of our lit shaders is logged
  debug-velocity --scene modern_velocity passes with
                 KISAK_VK_MODERN_DEBUG=velocity, where the velocity debug
                 view colours the moving band (the scene's own points)
  normals        --scene modern_normals passes with
                 KISAK_VK_MODERN_DEBUG=normals (and with linear light off),
                 where the normals debug view shows the normals buffer built
                 from the depth prepass (the scene's own points), with
                 KISAK_VK_MODERN_NORMALS=1, where it is built and not shown,
                 or KISAK_VK_MODERN_AO=1, where it is built for ambient
                 occlusion, and with KISAK_VK_MODERN_NORMALS=0 beside the
                 debug view or KISAK_VK_MODERN_AO=0, where it is not built;
                 the full run builds it for ambient occlusion (4 scenes)
                 unless linear light, lit inputs or our lit shaders are off
  ao             --scene modern_ao passes as it is, where our lit shader
                 darkens the floor in the corner by the ambient occlusion
                 (the scene's own points), with KISAK_VK_MODERN_DEBUG=ao (and
                 with linear light off), where the ambient occlusion debug
                 view shows open floor and wall white and the corner between
                 them darker, and the floor behind a thin hovering plate
                 light (with KISAK_VK_MODERN_AO_THICKNESS=1000 dark, as
                 behind a solid block), with KISAK_VK_MODERN_AO=1, and with
                 KISAK_VK_MODERN_AO=0, KISAK_VK_MODERN_NORMALS=0 (beside the
                 debug view or not), KISAK_VK_MODERN_LINEAR=0 or
                 KISAK_VK_MODERN_SHADERS=0, where none is built; the full
                 run builds it for modern_normals and modern_ao
  reflections    --scene modern_reflections passes with
                 KISAK_VK_MODERN_DEBUG=reflections, where the reflections
                 debug view shows the screen-space reflections traced in its
                 second scene (the scene's own points), with
                 KISAK_VK_MODERN_SSR=1, where they are traced and not shown,
                 and with the debug view beside KISAK_VK_MODERN_LINEAR=0
                 (logged, not traced), KISAK_VK_MODERN_SSR=0 and
                 KISAK_VK_MODERN_NORMALS=0, where they are not; the full run
                 traces none

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
SCENES = ('adapter', 'states', 'draw', 'render_target', 'shadow_map', 'model_lighting', 'dirty_update',
          'references', 'threads', 'queries', 'occlusion', 'frame_loop', 'gpu_feed', 'tuning', 'slow_frames',
          'kept_state', 'multisample', 'modern_taa', 'modern_linear', 'modern_bloom', 'modern_effects',
          'modern_lit', 'modern_lit_shader', 'modern_tonemap', 'modern_exposure', 'modern_velocity',
          'modern_normals', 'modern_ao', 'modern_reflections', 'reset',
          'fullscreen', 'present')


def run(binary, args, env, timeout=300):
    result = subprocess.run([binary] + args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=timeout)
    return result.returncode, result.stdout, result.stderr


def last_line(pattern, stdout):
    """The last match of a log-line pattern: the totals at the device's end
    come after any statistics a scene logs on its way (modern_exposure logs
    them inside a pass)."""
    found = None
    for found in re.finditer(pattern, stdout, re.M):
        pass
    return found


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
    # Auto exposure would move the other scenes' colours: off unless the
    # caller sets it; the exposure variant runs modern_exposure with it on.
    env.setdefault('KISAK_VK_MODERN_EXPOSURE', '0')
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

    # linear-off
    off = dict(env)
    off['KISAK_VK_MODERN_LINEAR'] = '0'
    code, stdout, _ = run(binary, ['--scene', 'modern_linear', '--validation', args.validation], off)
    found, scenes = parse(stdout)
    checks.expect('modern_linear passes with linear light off',
                  code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_linear']
                  and 'scenes in linear light' not in stdout, f'status {code}\n{stdout}')

    # bloom-off
    for name in ('KISAK_VK_MODERN_BLOOM', 'KISAK_VK_MODERN_LINEAR'):
        off = dict(env)
        off[name] = '0'
        code, stdout, _ = run(binary, ['--scene', 'modern_bloom', '--validation', args.validation], off)
        found, scenes = parse(stdout)
        checks.expect(f'modern_bloom passes with {name}=0',
                      code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_bloom']
                      and 'scenes with bloom' not in stdout, f'status {code}\n{stdout}')

    # effects-linear
    linear_fx = dict(env)
    linear_fx['KISAK_VK_MODERN_EFFECTS'] = 'linear'
    code, stdout, _ = run(binary, ['--scene', 'modern_effects', '--validation', args.validation], linear_fx)
    found, scenes = parse(stdout)
    checks.expect('modern_effects passes with KISAK_VK_MODERN_EFFECTS=linear',
                  code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_effects'],
                  f'status {code}\n{stdout}')

    # tonemap
    for curve in ('clamp', 'agx', 'aces', 'filmic'):
        chosen = dict(env)
        chosen['KISAK_VK_MODERN_TONEMAP'] = curve
        code, stdout, _ = run(binary, ['--scene', 'modern_tonemap', '--validation', args.validation], chosen)
        found, scenes = parse(stdout)
        logged = re.search(r'^d3d9_check\.log=vulkan d3d9: modern: tonemap curve (\w+)( \(KISAK_VK_MODERN_TONEMAP '
                           r'names no curve)?', stdout, re.M)
        want = 'neutral' if curve == 'filmic' else curve
        linear_off = env.get('KISAK_VK_MODERN_LINEAR') == '0'
        checks.expect(f'modern_tonemap passes with KISAK_VK_MODERN_TONEMAP={curve}',
                      code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_tonemap']
                      and (linear_off or (logged is not None and logged.group(1) == want
                                          and bool(logged.group(2)) == (curve == 'filmic'))),
                      f'status {code}\n{stdout}')

    # exposure
    exposed = dict(env)
    exposed['KISAK_VK_MODERN_EXPOSURE'] = '1'
    code, stdout, _ = run(binary, ['--scene', 'modern_exposure', '--validation', args.validation], exposed)
    found, scenes = parse(stdout)
    measured = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with auto exposure \((\d+) taken at '
                         r'once\), exposure (-?[0-9.]+) EV', stdout)
    linear_off = env.get('KISAK_VK_MODERN_LINEAR') == '0'
    checks.expect('modern_exposure passes with KISAK_VK_MODERN_EXPOSURE=1',
                  code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_exposure']
                  and (measured is None if linear_off
                       else measured is not None and measured.group(1) == '5' and measured.group(2) == '4'),
                  f'status {code}\n{stdout}')

    # lit-off
    for name in ('KISAK_VK_MODERN_LIT', 'KISAK_VK_MODERN_LINEAR'):
        off = dict(env)
        off[name] = '0'
        code, stdout, _ = run(binary, ['--scene', 'modern_lit', '--validation', args.validation], off)
        found, scenes = parse(stdout)
        checks.expect(f'modern_lit passes with {name}=0',
                      code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_lit']
                      and 'lit surfaces in linear light' not in stdout, f'status {code}\n{stdout}')
    # shaders-off
    off = dict(env)
    off['KISAK_VK_MODERN_SHADERS'] = '0'
    code, stdout, _ = run(binary, ['--scene', 'modern_lit_shader', '--validation', args.validation], off)
    found, scenes = parse(stdout)
    checks.expect('modern_lit_shader passes with KISAK_VK_MODERN_SHADERS=0',
                  code == 0 and found.get('d3d9_check') == 'passed'
                  and [s[0] for s in scenes] == ['modern_lit_shader'] and 'lit shaders of our own' not in stdout,
                  f'status {code}\n{stdout}')
    # debug-velocity
    debug = dict(env)
    debug['KISAK_VK_MODERN_DEBUG'] = 'velocity'
    code, stdout, _ = run(binary, ['--scene', 'modern_velocity', '--validation', args.validation], debug)
    found, scenes = parse(stdout)
    checks.expect('modern_velocity passes with KISAK_VK_MODERN_DEBUG=velocity',
                  code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_velocity']
                  and 'no velocity debug view' not in stdout and 'velocity debug view is not drawn' not in stdout,
                  f'status {code}\n{stdout}')

    # normals
    normals_line = re.compile(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with a normals buffer from the '
                              r'depth prepass, ([0-9.]+) ms GPU each$', re.M)
    # Our lit shaders read ambient occlusion, which reads the normals buffer.
    lit_shaders_on = all(env.get(name) != '0'
                         for name in ('KISAK_VK_MODERN_LINEAR', 'KISAK_VK_MODERN_LIT', 'KISAK_VK_MODERN_SHADERS'))
    for settings, built in (({'KISAK_VK_MODERN_DEBUG': 'normals'}, True),
                            ({'KISAK_VK_MODERN_DEBUG': 'normals', 'KISAK_VK_MODERN_LINEAR': '0'}, True),
                            ({'KISAK_VK_MODERN_NORMALS': '1'}, True),
                            ({'KISAK_VK_MODERN_AO': '1'}, True),
                            ({}, lit_shaders_on),
                            ({'KISAK_VK_MODERN_AO': '0'}, False),
                            ({'KISAK_VK_MODERN_DEBUG': 'normals', 'KISAK_VK_MODERN_NORMALS': '0'}, False)):
        chosen = dict(env)
        chosen.pop('KISAK_VK_MODERN_NORMALS', None)
        chosen.pop('KISAK_VK_MODERN_AO', None)
        chosen.update(settings)
        code, stdout, _ = run(binary, ['--scene', 'modern_normals', '--validation', args.validation], chosen)
        found, scenes = parse(stdout)
        line = normals_line.search(stdout)
        named = ' '.join(f'{k}={v}' for k, v in settings.items()) or 'the defaults'
        checks.expect(f'modern_normals passes with {named}',
                      code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_normals']
                      and (line is not None and line.group(1) == '1' if built else line is None)
                      and 'no normals buffer' not in stdout and 'normals debug view is not drawn' not in stdout,
                      f'status {code}\n{stdout}')

    # ao
    ao_line = re.compile(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with ambient occlusion \(radius '
                         r'48\.0, thickness ([0-9.]+), power 2\.00\), ([0-9.]+) ms GPU each$', re.M)
    for settings, built in (({}, True),
                            ({'KISAK_VK_MODERN_DEBUG': 'ao'}, True),
                            ({'KISAK_VK_MODERN_DEBUG': 'ao', 'KISAK_VK_MODERN_AO_THICKNESS': '1000'}, True),
                            ({'KISAK_VK_MODERN_DEBUG': 'ao', 'KISAK_VK_MODERN_LINEAR': '0'}, True),
                            ({'KISAK_VK_MODERN_AO': '1'}, True),
                            ({'KISAK_VK_MODERN_AO': '0'}, False),
                            ({'KISAK_VK_MODERN_NORMALS': '0'}, False),
                            ({'KISAK_VK_MODERN_LINEAR': '0'}, False),
                            ({'KISAK_VK_MODERN_SHADERS': '0'}, False),
                            ({'KISAK_VK_MODERN_DEBUG': 'ao', 'KISAK_VK_MODERN_AO': '0'}, False),
                            ({'KISAK_VK_MODERN_DEBUG': 'ao', 'KISAK_VK_MODERN_NORMALS': '0'}, False)):
        chosen = dict(env)
        for name in ('KISAK_VK_MODERN_NORMALS', 'KISAK_VK_MODERN_AO', 'KISAK_VK_MODERN_AO_RADIUS',
                     'KISAK_VK_MODERN_AO_POWER', 'KISAK_VK_MODERN_AO_THICKNESS', 'KISAK_VK_MODERN_LINEAR', 'KISAK_VK_MODERN_LIT',
                     'KISAK_VK_MODERN_SHADERS', 'KISAK_VK_MODERN_DEBUG'):
            chosen.pop(name, None)
        chosen.update(settings)
        code, stdout, _ = run(binary, ['--scene', 'modern_ao', '--validation', args.validation], chosen)
        found, scenes = parse(stdout)
        line = ao_line.search(stdout)
        thickness = '1000.0' if settings.get('KISAK_VK_MODERN_AO_THICKNESS') == '1000' else '8.0'
        named = ' '.join(f'{k}={v}' for k, v in settings.items()) or 'the defaults'
        checks.expect(f'modern_ao passes with {named}',
                      code == 0 and found.get('d3d9_check') == 'passed' and [s[0] for s in scenes] == ['modern_ao']
                      and (line is not None and line.group(1) == '1' and line.group(2) == thickness
                           if built else line is None)
                      and 'no ambient occlusion' not in stdout,
                      f'status {code}\n{stdout}')

    # reflections
    reflections_line = re.compile(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with screen-space '
                                  r'reflections \((\d+) levels\), ([0-9.]+) ms GPU each$', re.M)
    for settings, traced in (({'KISAK_VK_MODERN_DEBUG': 'reflections'}, True),
                             ({'KISAK_VK_MODERN_SSR': '1'}, True),
                             ({'KISAK_VK_MODERN_DEBUG': 'reflections', 'KISAK_VK_MODERN_LINEAR': '0'}, False),
                             ({'KISAK_VK_MODERN_DEBUG': 'reflections', 'KISAK_VK_MODERN_SSR': '0'}, False),
                             ({'KISAK_VK_MODERN_DEBUG': 'reflections', 'KISAK_VK_MODERN_NORMALS': '0'}, False)):
        chosen = dict(env)
        for name in ('KISAK_VK_MODERN_NORMALS', 'KISAK_VK_MODERN_SSR', 'KISAK_VK_MODERN_LINEAR'):
            chosen.pop(name, None)
        chosen.update(settings)
        code, stdout, _ = run(binary, ['--scene', 'modern_reflections', '--validation', args.validation], chosen)
        found, scenes = parse(stdout)
        line = reflections_line.search(stdout)
        named = ' '.join(f'{k}={v}' for k, v in settings.items())
        # Only linear light off is a reason to log; the others never try.
        logged = 'no screen-space reflections: the scene is not drawn in linear light' in stdout
        checks.expect(f'modern_reflections passes with {named}',
                      code == 0 and found.get('d3d9_check') == 'passed'
                      and [s[0] for s in scenes] == ['modern_reflections']
                      and (line is not None and line.group(1) == '1' and line.group(2) == '5' if traced
                           else line is None)
                      and logged == (settings.get('KISAK_VK_MODERN_LINEAR') == '0'
                                     and settings.get('KISAK_VK_MODERN_SSR') != '0')
                      and 'reflections debug view is not drawn' not in stdout
                      and 'no reflections debug view' not in stdout,
                      f'status {code}\n{stdout}')

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
    # The Modern path's lines at teardown: the modern_taa scene's frames (4
    # still, 16 jittered; the first of each starts the history again), the
    # modern_linear scene's one, the modern_bloom scene's three, the
    # modern_effects scene's one, the modern_lit, modern_lit_shader and
    # modern_tonemap scenes' one each (all cuts), the modern_exposure scene's
    # five (four cuts), and the modern_velocity scene's (three runs of 8, each
    # starting with a cut); those thirteen before modern_velocity were drawn
    # in linear light, with one copy read from
    # them, two of modern_bloom's with bloom and modern_lit's,
    # modern_lit_shader's and modern_tonemap's with lit surfaces (six pixel
    # modules of modern_lit_shader's built as our own lit shaders, unless
    # KISAK_VK_MODERN_SHADERS=0), unless KISAK_VK_MODERN_LINEAR=0 (or
    # KISAK_VK_MODERN_BLOOM=0 for the bloom, KISAK_VK_MODERN_LIT=0 for the lit
    # surfaces); in the second and third modern_velocity runs the 7 scenes
    # after the cut each had one surface that moves on its own, in the third
    # a skinned one, whose 128 vertices every scene of that run kept. Last,
    # the modern_normals scene's one (a cut), the modern_ao scene's one (a
    # cut) and the modern_reflections scene's two (the first a cut), in
    # linear light unless KISAK_VK_MODERN_LINEAR=0.
    modern = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes anti-aliased \((\d+) without history\)',
                       stdout)
    checks.expect('the Modern path anti-aliased 61 scenes, 20 without history',
                  modern is not None and modern.group(1) == '61' and modern.group(2) == '20',
                  modern.group(0) if modern else 'missing')
    velocity = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with surfaces that move on their own '
                         r'\((\d+) surfaces\), ([0-9.]+) ms GPU each for their velocity$', stdout)
    checks.expect('the Modern path drew the velocity of 14 surfaces in 14 scenes',
                  velocity is not None and velocity.group(1) == '14' and velocity.group(2) == '14',
                  velocity.group(0) if velocity else 'missing')
    kept = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes kept skinned vertices \((\d+) surfaces, '
                     r'([0-9.]+) KB each scene\), (\d+) skinned surfaces drawn from last scene\'s$', stdout)
    checks.expect("the Modern path kept a skinned surface's vertices in 8 scenes and drew 7 from them",
                  kept is not None and kept.group(1) == '8' and kept.group(2) == '8' and kept.group(3) == '4.0'
                  and kept.group(4) == '7',
                  kept.group(0) if kept else 'missing')
    if env.get('KISAK_VK_MODERN_EXPOSURE') == '0':
        checks.expect('no auto exposure with KISAK_VK_MODERN_EXPOSURE=0', 'scenes with auto exposure' not in stdout)
    # Ambient occlusion, and the normals buffer for it, in the four scenes
    # that draw a depth prepass (modern_normals, modern_ao and
    # modern_reflections' two), when our lit shaders read it.
    if 'KISAK_VK_MODERN_NORMALS' not in env and 'KISAK_VK_MODERN_DEBUG' not in env:
        read = env.get('KISAK_VK_MODERN_AO') != '0' and (
            env.get('KISAK_VK_MODERN_AO') == '1'
            or all(env.get(name) != '0'
                   for name in ('KISAK_VK_MODERN_LINEAR', 'KISAK_VK_MODERN_LIT', 'KISAK_VK_MODERN_SHADERS')))
        normals = last_line(normals_line.pattern, stdout)
        occlusion = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with ambient occlusion \(radius ',
                              stdout)
        if read:
            checks.expect('a normals buffer and ambient occlusion for the 4 scenes with a depth prepass',
                          normals is not None and normals.group(1) == '4' and occlusion is not None
                          and occlusion.group(1) == '4',
                          f'{normals.group(0) if normals else "no normals line"}; '
                          f'{occlusion.group(0) if occlusion else "no ambient occlusion line"}')
        else:
            checks.expect('no normals buffer when nothing reads it', normals is None)
            checks.expect('no ambient occlusion when nothing reads it', occlusion is None)
    if 'KISAK_VK_MODERN_SSR' not in env and 'KISAK_VK_MODERN_DEBUG' not in env:
        checks.expect('no screen-space reflections when nothing reads them',
                      'scenes with screen-space reflections' not in stdout)
    linear = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes in linear light \((\d+) copies read '
                       r'from them\), ([0-9.]+) ms GPU each for the tonemap$', stdout)
    if env.get('KISAK_VK_MODERN_LINEAR') == '0':
        checks.expect('no scene in linear light with KISAK_VK_MODERN_LINEAR=0', linear is None,
                      linear.group(0) if linear else '')
    else:
        checks.expect('the Modern path drew 17 scenes in linear light and read 1 copy from them',
                      linear is not None and linear.group(1) == '17' and linear.group(2) == '1',
                      linear.group(0) if linear else 'missing')
    bloom = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with bloom in place of the glow '
                      r'\(([0-9.]+) passes each\), ([0-9.]+) ms GPU each$', stdout)
    if env.get('KISAK_VK_MODERN_LINEAR') == '0' or env.get('KISAK_VK_MODERN_BLOOM') == '0':
        checks.expect('no scene with bloom with linear light or bloom off', bloom is None,
                      bloom.group(0) if bloom else '')
    else:
        checks.expect('the Modern path drew bloom for 2 scenes, 7 passes each',
                      bloom is not None and bloom.group(1) == '2' and bloom.group(2) == '7.0',
                      bloom.group(0) if bloom else 'missing')
    lit = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: (\d+) scenes with lit surfaces in linear light, '
                    r'kept over white \(([0-9.]+) changes of lit inputs each\)$', stdout)
    if env.get('KISAK_VK_MODERN_LINEAR') == '0' or env.get('KISAK_VK_MODERN_LIT') == '0':
        checks.expect('no scene with lit surfaces in linear light with linear light or lit inputs off', lit is None,
                      lit.group(0) if lit else '')
    else:
        checks.expect('the Modern path drew lit surfaces in linear light in 4 scenes, inputs changed 2.8 times each',
                      lit is not None and lit.group(1) == '4' and lit.group(2) == '2.8',
                      lit.group(0) if lit else 'missing')
    shaders = last_line(r'^d3d9_check\.log=vulkan d3d9: modern: lit shaders of our own: ([0-9.]+) changes to one '
                        r'each scene, (\d+) pixel modules built as one \((\d+) could not be\)', stdout)
    if (env.get('KISAK_VK_MODERN_LINEAR') == '0' or env.get('KISAK_VK_MODERN_LIT') == '0'
            or env.get('KISAK_VK_MODERN_SHADERS') == '0'):
        checks.expect('no lit shaders of our own with linear light, lit inputs or our shaders off', shaders is None,
                      shaders.group(0) if shaders else '')
    else:
        # modern_ao's floor tests depth against its prepass: a depth compare
        # of its own, built into its module unless chosen at run time.
        modules = '6' if env.get('KISAK_VK_DEPTH_COMPARE') == 'dynamic' else '7'
        checks.expect(f'the Modern path built {modules} pixel modules as our own lit shaders, none refused',
                      shaders is not None and shaders.group(2) == modules and shaders.group(3) == '0',
                      shaders.group(0) if shaders else 'missing')
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
    # The translated shaders' float rule (KISAK_VK_STRICT_FLOATS=1: strict).
    floats = re.search(r'legacy: device ready: .*, floats (fast|strict), ', stdout, re.M)
    want_floats = 'strict' if env.get('KISAK_VK_STRICT_FLOATS') == '1' else 'fast'
    checks.expect(f'the device logs floats {want_floats}', floats is not None and floats.group(1) == want_floats,
                  floats.group(1) if floats else 'missing')
    # The legacy state rule (KISAK_VK_LEGACY_STATE=dynamic: read at run time).
    state = re.search(r'legacy: device ready: .*, legacy state (fixed|dynamic), ', stdout, re.M)
    want_state = 'dynamic' if env.get('KISAK_VK_LEGACY_STATE') == 'dynamic' else 'fixed'
    checks.expect(f'the device logs legacy state {want_state}', state is not None and state.group(1) == want_state,
                  state.group(1) if state else 'missing')
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
