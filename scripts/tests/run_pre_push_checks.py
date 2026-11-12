#!/usr/bin/env python3
"""Run the asset-free checks that every push should pass, in parallel.

    python3 scripts/tests/run_pre_push_checks.py [--list] [--native] [--jobs N]

These are the checks a hosted run repeats on every pull request, so a failure
found here saves a full wait in the CI queue: the Linux bootstrap test, the
runner self-tests, the workflow contract test and the CI script tests. With
--native (or when build-linux/ and bin/KisakCOD-dedi exist) the full asset-free
native set, run_linux_checks.py, runs as well. Local results are not CI
evidence; they only tell you what to fix before pushing.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / 'scripts/tests'

CHECKS = [
    'test_linux_bootstrap.py',
    'test_linux_check_runner.py',
    'test_free_camera_check_runner.py',
    'test_native_d3d9_boundary_runner.py',
    'test_world_frontend_check_runner.py',
    'test_world_surface_check_runner.py',
    'test_linux_window_workflow.py',
    'test_ci_apt_install.py',
    'test_ci_each_config.py',
    'test_changed_units.py',
]
NATIVE = ['run_linux_checks.py', '--build-dir', 'build-linux', '--binary', 'bin/KisakCOD-dedi']


def native_available():
    return (ROOT / 'build-linux').is_dir() and (ROOT / 'bin/KisakCOD-dedi').is_file()


def commands(native):
    found = [[sys.executable, str(TESTS / name)] for name in CHECKS]
    if native:
        found.append([sys.executable, str(TESTS / NATIVE[0]), *NATIVE[1:]])
    return found


def run_one(command):
    started = time.monotonic()
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    return command, result, time.monotonic() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--list', action='store_true', help='print the checks and exit')
    parser.add_argument('--native', action='store_true',
                        help='also run run_linux_checks.py (needs build-linux/ and bin/KisakCOD-dedi)')
    parser.add_argument('--jobs', type=int, default=4, help='checks to run at once (default 4)')
    args = parser.parse_args()
    native = args.native or native_available()
    if args.native and not native_available():
        print('run_pre_push_checks: --native needs build-linux/ and bin/KisakCOD-dedi', file=sys.stderr)
        return 2
    planned = commands(native)
    if args.list:
        for command in planned:
            print(' '.join(Path(part).name if part.endswith('.py') else part
                           for part in command[1:]))
        return 0
    if not native:
        print('run_pre_push_checks: run_linux_checks.py not run (no build-linux/ or bin/KisakCOD-dedi)')
    failed = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for command, result, seconds in pool.map(run_one, planned):
            name = Path(command[1]).name
            print(f'{"ok  " if result.returncode == 0 else "FAIL"} {name} ({seconds:.0f} s)')
            if result.returncode != 0:
                failed.append(name)
                print((result.stdout + result.stderr)[-3000:])
    if failed:
        print(f'run_pre_push_checks: {len(failed)} of {len(planned)} failed: {", ".join(failed)}')
        return 1
    print(f'run_pre_push_checks: all {len(planned)} passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
