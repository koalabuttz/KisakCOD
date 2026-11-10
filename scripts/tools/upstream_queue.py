#!/usr/bin/env python3
"""Check and apply the upstream patch queue in docs/upstream.

The queue holds changes prepared for the upstream repository, one patch per
future pull request, each made on upstream's own tree (docs/upstream/README.md).
Nothing here sends anything anywhere: the tool only reads the patches and a
local checkout of upstream.

  upstream_queue.py lint
      Check every patch's message and paths against the queue rules.
  upstream_queue.py check --upstream DIR
      lint, then check that DIR is at the pinned base commit, that every patch
      applies to it alone (or on top of the patches DEPENDS names for it), and
      that the whole queue applies in order.
  upstream_queue.py apply --upstream DIR
      Apply the whole queue to DIR in order and leave it applied (for a build).
"""

import argparse
import email
import email.policy
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
QUEUE = ROOT / 'docs' / 'upstream'

# Upstream's own title style: "fix: ...", "refactor(database): ...".
SUBJECT = re.compile(r'^(fix|security|refactor|feat|perf|build|ci|docs)(\([a-z0-9_-]+\))?: \S.*$')
MAX_SUBJECT = 80
MAX_BODY_LINES = 12

# Text that must not reach an upstream commit message. A bare "#N" links the
# commit to an issue or pull request of whichever repository shows it.
BANNED = (
    (re.compile(r'#\d'), 'an issue or pull request number written with #'),
    (re.compile(r'Claude-Session', re.I), 'a session trailer'),
    (re.compile(r'claude\.ai', re.I), 'a session link'),
    (re.compile(r'koalabuttz/', re.I), 'a link into the fork'),
    (re.compile(r'\bfork PR\b', re.I), 'a fork pull request number'),
    (re.compile(r'\blaptop\b', re.I), "a note about the fork's test machines"),
)

# Paths that exist only in the fork. A patch touching one was not made on
# upstream's tree.
FORK_ONLY = ('src/linux/', 'scripts/tests/', 'scripts/tools/', 'tests/linux_', 'docs/')


class QueueError(Exception):
    pass


def patch_files(queue):
    return sorted((queue / 'patches').glob('*.patch'))


def read_base(queue):
    base = (queue / 'BASE').read_text().split()
    if len(base) != 1 or not re.fullmatch(r'[0-9a-f]{40}', base[0]):
        raise QueueError('BASE must hold one full commit id')
    return base[0]


def read_depends(queue, patches):
    """Return {patch number: [numbers it is made on top of]} from DEPENDS.

    Most patches are made on the base alone and are not listed. A line
    "0015: 0014" says 0015 was made on top of 0014 and can only be offered
    once 0014 is in.
    """
    path = queue / 'DEPENDS'
    depends = {}
    if not path.exists():
        return depends
    known = [patch.name[:4] for patch in patches]
    for line in path.read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        match = re.fullmatch(r'(\d{4}):((?: \d{4})+)', line)
        if not match:
            raise QueueError('DEPENDS: cannot read %r' % line)
        number, needs = match.group(1), match.group(2).split()
        if number not in known:
            raise QueueError('DEPENDS: there is no patch %s' % number)
        if number in depends:
            raise QueueError('DEPENDS: %s is listed twice' % number)
        for need in needs:
            if need not in known or need >= number:
                raise QueueError('DEPENDS: %s can only be made on an earlier patch, not %s' % (number, need))
        depends[number] = needs
    return depends


def needed_before(number, depends):
    """Every patch NUMBER is made on top of, directly or not, in queue order."""
    found = set()
    todo = list(depends.get(number, ()))
    while todo:
        need = todo.pop()
        if need not in found:
            found.add(need)
            todo.extend(depends.get(need, ()))
    return sorted(found)


def split_patch(text):
    """Return (subject, body lines, touched paths) of one format-patch file."""
    message = email.message_from_string(text, policy=email.policy.default)
    subject = re.sub(r'^\[PATCH[^\]]*\]\s*', '', ' '.join(str(message['Subject'] or '').split()))
    payload = message.get_payload()
    body = payload.split('\n---\n', 1)[0] if '\n---\n' in payload else payload.split('\ndiff --git ', 1)[0]
    paths = re.findall(r'^diff --git a/(\S+) b/\S+$', text, re.M)
    return subject, body.strip('\n').split('\n'), paths


def lint_patch(name, text):
    """Return the list of rule violations in one patch."""
    problems = []
    subject, body, paths = split_patch(text)
    if not SUBJECT.match(subject):
        problems.append("subject is not in upstream's 'fix: ...' style: %r" % subject)
    if len(subject) > MAX_SUBJECT:
        problems.append('subject is longer than %d characters' % MAX_SUBJECT)
    used = [line for line in body if line.strip()]
    if not used:
        problems.append('the message has no body')
    if len(used) > MAX_BODY_LINES:
        problems.append('body has %d lines; keep it to %d' % (len(used), MAX_BODY_LINES))
    message = '\n'.join([subject] + body)
    for pattern, what in BANNED:
        if pattern.search(message):
            problems.append('message contains %s' % what)
    if not paths:
        problems.append('no file changes found')
    for path in paths:
        if path.startswith(FORK_ONLY):
            problems.append('touches a fork-only path: %s' % path)
    if not re.fullmatch(r'\d{4}-[a-z0-9_.-]+\.patch', name):
        problems.append('file name is not NNNN-topic.patch')
    return problems


def lint(queue):
    read_base(queue)
    patches = patch_files(queue)
    if not patches:
        raise QueueError('the queue has no patches')
    failed = False
    for patch in patches:
        for problem in lint_patch(patch.name, patch.read_text()):
            failed = True
            print('%s: %s' % (patch.name, problem))
    if failed:
        raise QueueError('the queue breaks its rules')
    read_depends(queue, patches)
    return patches


def git(upstream, *args, check=True):
    return subprocess.run(['git', '-C', str(upstream)] + list(args), check=check,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def require_clean_base(queue, upstream):
    base = read_base(queue)
    head = git(upstream, 'rev-parse', 'HEAD').stdout.strip()
    if head != base:
        raise QueueError('%s is at %s, not the pinned base %s' % (upstream, head, base))
    if git(upstream, 'status', '--porcelain').stdout.strip():
        raise QueueError('%s has local changes' % upstream)


def apply_all(patches, upstream):
    for patch in patches:
        result = git(upstream, 'apply', '--whitespace=nowarn', str(patch), check=False)
        if result.returncode:
            raise QueueError('%s does not apply after the patches before it:\n%s' % (patch.name, result.stdout))


def reset(upstream):
    git(upstream, 'reset', '--hard', '-q', 'HEAD')
    git(upstream, 'clean', '-fdq')


def check(queue, upstream):
    patches = lint(queue)
    depends = read_depends(queue, patches)
    by_number = {patch.name[:4]: patch for patch in patches}
    require_clean_base(queue, upstream)
    stacked = 0
    for patch in patches:
        before = needed_before(patch.name[:4], depends)
        try:
            apply_all([by_number[number] for number in before], upstream)
            result = git(upstream, 'apply', '--check', '--whitespace=nowarn', str(patch), check=False)
        finally:
            if before:
                reset(upstream)
        if result.returncode:
            on = 'the base plus %s' % ', '.join(before) if before else 'the base alone'
            raise QueueError('%s does not apply to %s:\n%s' % (patch.name, on, result.stdout))
        stacked += bool(before)
    try:
        apply_all(patches, upstream)
    finally:
        reset(upstream)
    print('%d patches: rules kept, %d apply to %s alone, %d on the patches DEPENDS names, all apply in order'
          % (len(patches), len(patches) - stacked, read_base(queue)[:7], stacked))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('lint', 'check', 'apply'))
    parser.add_argument('--upstream', type=pathlib.Path, help='a checkout of upstream at the pinned base')
    parser.add_argument('--queue', type=pathlib.Path, default=QUEUE)
    args = parser.parse_args(argv)
    try:
        if args.command == 'lint':
            print('%d patches keep the rules' % len(lint(args.queue)))
            return 0
        if args.upstream is None:
            parser.error('%s needs --upstream' % args.command)
        if args.command == 'check':
            check(args.queue, args.upstream)
        else:
            patches = lint(args.queue)
            require_clean_base(args.queue, args.upstream)
            apply_all(patches, args.upstream)
            print('applied %d patches to %s' % (len(patches), args.upstream))
        return 0
    except QueueError as error:
        print('upstream queue: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
