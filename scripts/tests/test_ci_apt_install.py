#!/usr/bin/env python3
"""Exercise CI mirror selection and installer failures without sudo or network."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ubuntu_mirror', ROOT / 'scripts/ci/ubuntu-mirror.py')
mirror = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirror)


class MirrorTests(unittest.TestCase):
    def test_both_source_formats_preserve_repository_options(self):
        with tempfile.TemporaryDirectory() as temp:
            apt = Path(temp)
            (apt / 'sources.list.d').mkdir()
            fixtures = {
                'sources.list': (
                    'deb [arch=amd64,i386 signed-by=/key.gpg] '
                    'http://azure.archive.ubuntu.com/ubuntu noble main universe\n'),
                'sources.list.d/ubuntu.sources': (
                    'Types: deb deb-src\n'
                    'URIs: https://azure.archive.ubuntu.com/ubuntu/\n'
                    'Suites: noble noble-updates noble-backports\n'
                    'Components: main restricted universe multiverse\n'
                    'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n'),
                'sources.list.d/extra.list': (
                    'deb http://azure.archive.ubuntu.com/ubuntu noble-security main\n'),
            }
            for name, content in fixtures.items():
                (apt / name).write_text(content)
            mirror.use_main_archive(apt)
            for name, content in fixtures.items():
                expected = content.replace('http://azure.archive.ubuntu.com/ubuntu',
                                           'https://archive.ubuntu.com/ubuntu')
                expected = expected.replace('https://azure.archive.ubuntu.com/ubuntu',
                                            'https://archive.ubuntu.com/ubuntu')
                self.assertEqual((apt / name).read_text(), expected)

    def test_arm_ports_security_and_third_party_sources_stay_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            apt = Path(temp)
            path = apt / 'sources.list'
            original = (
                'deb http://ports.ubuntu.com/ubuntu-ports noble main\n'
                'deb http://security.ubuntu.com/ubuntu noble-security main\n'
                'deb https://packages.microsoft.com/repos/code stable main\n'
                'deb https://azure.archive.ubuntu.com/ubuntu-other noble main\n'
                'deb https://azure.archive.ubuntu.com.example/ubuntu noble main\n')
            path.write_text(original)
            before = path.stat().st_mtime_ns
            mirror.use_main_archive(apt)
            self.assertEqual(path.read_text(), original)
            self.assertEqual(path.stat().st_mtime_ns, before)

    def test_hosted_runner_mirror_list_is_rewritten_without_losing_priorities(self):
        with tempfile.TemporaryDirectory() as temp:
            apt = Path(temp)
            (apt / 'sources.list.d').mkdir()
            source = apt / 'sources.list.d/ubuntu.sources'
            content = ('Types: deb\nURIs: mirror+file:/etc/apt/apt-mirrors.txt\n'
                       'Suites: noble noble-updates noble-security\n'
                       'Components: main universe\nSigned-By: /ubuntu-keyring.gpg\n')
            source.write_text(content)
            mirrors = apt / 'apt-mirrors.txt'
            mirrors.write_text('http://azure.archive.ubuntu.com/ubuntu\tpriority:1\n'
                               'http://archive.ubuntu.com/ubuntu\tpriority:2\n')
            mirror.use_main_archive(apt)
            self.assertEqual(source.read_text(), content)
            self.assertEqual(mirrors.read_text(),
                             'https://archive.ubuntu.com/ubuntu\tpriority:1\n'
                             'http://archive.ubuntu.com/ubuntu\tpriority:2\n')

    def test_the_runner_shaped_list_is_rewritten_although_it_names_the_main_archive(self):
        # What a hosted runner ships (144 bytes): Azure first, then the main
        # archive and security over https. A list that already names the main
        # archive must still lose its Azure entry.
        original = ('http://azure.archive.ubuntu.com/ubuntu/\tpriority:1\n'
                    'https://archive.ubuntu.com/ubuntu/\tpriority:2\n'
                    'https://security.ubuntu.com/ubuntu/\tpriority:3\n')
        self.assertEqual(len(original.encode()), 144)
        with tempfile.TemporaryDirectory() as temp:
            apt = Path(temp)
            mirrors = apt / 'apt-mirrors.txt'
            mirrors.write_text(original)
            mirror.use_main_archive(apt)
            self.assertEqual(mirrors.read_text(),
                             'https://archive.ubuntu.com/ubuntu/\tpriority:1\n'
                             'https://archive.ubuntu.com/ubuntu/\tpriority:2\n'
                             'https://security.ubuntu.com/ubuntu/\tpriority:3\n')
            first = mirrors.read_text()
            mirror.use_main_archive(apt)
            self.assertEqual(mirrors.read_text(), first)


class InstallerTests(unittest.TestCase):
    def run_installer(self, args, fail='', times=None, dns_broken=False, resolver_fixes=True):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            sudo = folder / 'sudo'
            # The only privileged commands run by the script are recorded here;
            # no host apt configuration or installed package can be changed.
            sudo.write_text(f'#!{sys.executable}\n' + '''
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
entry = {'args': args}
if args[0] == 'tee':
    entry['stdin'] = sys.stdin.read()
with open(os.environ['FAKE_SUDO_LOG'], 'a') as log:
    log.write(json.dumps(entry) + '\\n')
if args[:2] == ['resolvectl', 'dns'] and not os.environ.get('FAKE_RESOLVER_STAYS_BROKEN'):
    (Path(os.environ['FAKE_SUDO_LOG']).with_suffix('.dns')).write_text('fixed')
if args[0] == 'mkdir':
    assert Path(args[-1]).is_relative_to(os.environ['RUNNER_TEMP'])
    Path(args[-1]).mkdir(parents=True, exist_ok=True)
apt_args = args[1:] if args[0] == 'DEBIAN_FRONTEND=noninteractive' else args
if apt_args[0] == 'apt-get' and apt_args[1] == os.environ.get('FAKE_APT_FAIL'):
    # FAKE_APT_FAIL_TIMES: fail that many times, then work (a lookup that heals).
    count_file = Path(os.environ['FAKE_SUDO_LOG']).with_suffix('.count')
    count = int(count_file.read_text()) if count_file.exists() else 0
    count_file.write_text(str(count + 1))
    if count < int(os.environ.get('FAKE_APT_FAIL_TIMES', '1000')):
        sys.exit(42)
''')
            sudo.chmod(0o755)
            # Name lookups: getent fails while FAKE_DNS_BROKEN is set and the
            # fake sudo has not been told to change the resolvers.
            getent = folder / 'getent'
            getent.write_text('''#!/bin/sh
[ -n "$FAKE_DNS_BROKEN" ] && [ ! -e "${FAKE_SUDO_LOG%.jsonl}.dns" ] && exit 2
exit 0
''')
            getent.chmod(0o755)
            ip = folder / 'ip'
            ip.write_text('#!/bin/sh\necho "default via 10.1.0.1 dev eth0 proto dhcp"\n')
            ip.chmod(0o755)
            log = folder / 'commands.jsonl'
            env = dict(os.environ, PATH=f'{folder}:{os.environ["PATH"]}',
                       RUNNER_TEMP=temp, FAKE_SUDO_LOG=str(log), FAKE_APT_FAIL=fail,
                       KISAK_APT_ATTEMPTS='3', KISAK_APT_RETRY_DELAY='0')
            if times is not None:
                env['FAKE_APT_FAIL_TIMES'] = str(times)
            if dns_broken:
                env['FAKE_DNS_BROKEN'] = '1'
            if not resolver_fixes:
                env['FAKE_RESOLVER_STAYS_BROKEN'] = '1'
            env.pop('KISAK_APT_ARCHIVES', None)
            result = subprocess.run(['bash', str(ROOT / 'scripts/ci/apt-install.sh'), *args],
                                    env=env, capture_output=True, text=True, timeout=10)
            entries = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            return result, entries, str(folder / 'apt-archives')

    def test_working_name_lookups_leave_the_resolvers_alone(self):
        result, entries, _ = self.run_installer(['curl'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse([e for e in entries if e['args'][0] == 'resolvectl'])

    def test_broken_name_lookups_switch_the_default_link_to_public_resolvers(self):
        result, entries, _ = self.run_installer(['curl'], dns_broken=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [e['args'] for e in entries]
        self.assertIn(['resolvectl', 'dns', 'eth0', '1.1.1.1', '8.8.8.8'], commands)
        self.assertIn(['resolvectl', 'flush-caches'], commands)
        self.assertIn('resolves through public resolvers', result.stderr)
        # The resolvers change before the first package refresh.
        self.assertLess(commands.index(['resolvectl', 'dns', 'eth0', '1.1.1.1', '8.8.8.8']),
                        commands.index(['apt-get', 'update']))

    def test_lookups_that_stay_broken_still_let_apt_report_the_outcome(self):
        result, entries, _ = self.run_installer(['curl'], dns_broken=True, resolver_fixes=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('still does not resolve', result.stderr)
        self.assertTrue([e for e in entries if e['args'][:2] == ['apt-get', 'install']
                         or e['args'][1:3] == ['apt-get', 'install']])

    def test_mirror_setup_precedes_update_and_install_keeps_requested_packages(self):
        result, entries, archives = self.run_installer(['clang', 'libsdl2-dev:i386'])
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [entry['args'] for entry in entries]
        setup = ['python3', str(ROOT / 'scripts/ci/ubuntu-mirror.py')]
        self.assertLess(commands.index(setup), commands.index(['apt-get', 'update']))
        install = next(cmd for cmd in commands if 'install' in cmd)
        self.assertEqual(install, ['DEBIAN_FRONTEND=noninteractive', 'apt-get', 'install', '-y',
                                   '-o', f'Dir::Cache::Archives={archives}', 'clang', 'libsdl2-dev:i386'])
        config = next(entry['stdin'] for entry in entries if entry['args'][0] == 'tee')
        self.assertIn('Acquire::Retries "5";', config)
        self.assertNotIn('AllowUnauthenticated', config)

    def test_second_install_uses_existing_lists(self):
        result, entries, _ = self.run_installer(['--no-update', 'g++-multilib'])
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [entry['args'] for entry in entries]
        self.assertNotIn(['apt-get', 'update'], commands)
        self.assertFalse(any(cmd[0] == 'python3' for cmd in commands))
        self.assertEqual(next(cmd for cmd in commands if 'install' in cmd)[-1], 'g++-multilib')

    def test_update_failure_stops_before_install(self):
        result, entries, _ = self.run_installer(['clang'], fail='update')
        self.assertEqual(result.returncode, 42)
        self.assertFalse(any('install' in entry['args'] for entry in entries))
        # It was tried three times (the tests' limit), and the mirror setup ran once.
        commands = [entry['args'] for entry in entries]
        self.assertEqual(commands.count(['apt-get', 'update']), 3)
        self.assertEqual(sum(cmd[0] == 'python3' for cmd in commands), 1)
        self.assertIn('failed after 3 attempt(s)', result.stderr)

    def test_a_failure_that_heals_is_retried(self):
        # Hosted runners lost name lookups for a minute on 2026-11-11.
        for fail in ('update', 'install'):
            with self.subTest(fail=fail):
                result, entries, _ = self.run_installer(['clang'], fail=fail, times=2)
                self.assertEqual(result.returncode, 0, result.stderr)
                commands = [entry['args'] for entry in entries]
                self.assertEqual(commands.count(['apt-get', 'update']), 3)
                self.assertTrue(any(cmd[-1] == 'clang' and 'install' in cmd for cmd in commands))
                self.assertTrue(any(entry['args'][0] == 'chown' for entry in entries))
                self.assertIn('trying again', result.stderr)

    def test_no_update_retries_only_the_install(self):
        result, entries, _ = self.run_installer(['--no-update', 'clang'], fail='install', times=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [entry['args'] for entry in entries]
        self.assertNotIn(['apt-get', 'update'], commands)
        self.assertEqual(sum('install' in cmd for cmd in commands), 2)

    def test_install_failure_is_not_hidden_by_cache_cleanup(self):
        result, entries, _ = self.run_installer(['clang'], fail='install')
        self.assertEqual(result.returncode, 42)
        self.assertFalse(any(entry['args'][0] == 'chown' for entry in entries))

    def test_missing_packages_exit_before_privileged_commands(self):
        for args in ([], ['--no-update']):
            with self.subTest(args=args):
                result, entries, _ = self.run_installer(args)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(entries, [])


if __name__ == '__main__':
    unittest.main()
