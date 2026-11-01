#!/usr/bin/env python3
"""Opt-in actual X11 readback for the engine viewport producer and UI clipping."""
import argparse
import json
import os
from pathlib import Path
import select
import shlex
import subprocess
from test_linux_window_x11 import wait_for, request_window_close, stop
from x11_pixel_checks import read_pixels


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary', type=Path)
    p.add_argument('--runner', default='')
    p.add_argument('--xvfb', default='Xvfb')
    p.add_argument('--xdotool', default='xdotool')
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    processes = []
    r, w = os.pipe()
    evidence = []
    try:
        with (a.output / 'xvfb.log').open('w') as log:
            xvfb = subprocess.Popen([a.xvfb, '-displayfd', str(w), '-screen', '0',
                                     '1200x900x24', '-nolisten', 'tcp'], pass_fds=(w,),
                                    stdout=log, stderr=subprocess.STDOUT)
        processes.append(xvfb)
        os.close(w)
        w = None
        if not select.select([r], [], [], 15)[0]:
            raise RuntimeError('Xvfb startup timeout')
        number = os.read(r, 64).decode().strip()
        assert number.isdecimal(), number
        display = ':' + number
        env = dict(os.environ, DISPLAY=display, SDL_VIDEODRIVER='x11')
        def xd(*args):
            return subprocess.check_output([a.xdotool, *map(str, args)], env=env,
                                           stderr=subprocess.STDOUT, text=True).strip()
        evidence.append({'preflight_geometry': xd('getdisplaygeometry')})
        for index, (vx, vy) in enumerate(((37, 29), (0, 0))):
            cwd = a.output / ('cwd-' + str(index))
            cwd.mkdir()
            path = a.output / ('client-' + str(index) + '.log')
            title = 'Kisak-viewport-' + str(os.getpid()) + '-' + str(index)
            with path.open('w') as log:
                client = subprocess.Popen(shlex.split(a.runner) + [str(a.binary.resolve()),
                    '--ui-viewport', str(vx), str(vy), '96', '64', '--trace',
                    '--run-ms', '60000', '--title', title], cwd=cwd, env=env,
                    stdout=log, stderr=subprocess.STDOUT)
            processes.append(client)
            def presented(width, height):
                return f'target_width={width} target_height={height} producer=CL_SetViewport' in path.read_text()
            wait_for(lambda: presented(960, 540), 20, 'initial viewport presentation', client)
            window = int(xd('search', '--name', '^' + title + '$').splitlines()[0])
            xd('windowmove', window, 0, 0)
            expected = {'tl': (0, 0, (160,64,95)), 'tr': (95,0,(32,192,95)),
                        'bl': (0,63,(32,64,223)), 'br': (95,63,(160,192,95)),
                        'center_tl': (47,31,(117,96,111)), 'center_tr': (48,31,(75,160,100)),
                        'center_bl': (47,32,(75,96,175)), 'center_br': (48,32,(117,160,122)),
                        'right_clip': (96,16,(64,128,191)), 'bottom_clip': (23,64,(64,128,191))}
            if vx and vy:
                expected.update(left_clip=(-1,16,(64,128,191)), top_clip=(23,-1,(64,128,191)))
            for width, height in ((960,540), (1100,700)):
                if width != 960:
                    xd('windowsize', window, width, height)
                    wait_for(lambda: presented(width,height), 20, 'resized viewport presentation', client)
                result = read_pixels(display, window, [(k,vx+x,vy+y) for k,(x,y,_) in expected.items()])
                assert (result['width'],result['height']) == (width,height), result
                for key, (_,_,rgb) in expected.items():
                    assert result['samples'][key]['rgb'] == list(rgb), (key,result['samples'][key],rgb)
                evidence.append({'viewport':[vx,vy,96,64], 'readback':result})
            request_window_close(display, window)
            assert client.wait(timeout=15) == 0, path.read_text()
            assert not list(cwd.iterdir()), 'client wrote into isolated cwd'
        (a.output / 'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
        print('PASS: 44 actual viewport pixels across two origins and two surface sizes')
    finally:
        for proc in reversed(processes):
            stop(proc)
        os.close(r)
        if w is not None:
            os.close(w)


if __name__ == '__main__':
    main()
