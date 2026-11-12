#!/usr/bin/env python3
"""Which translation units a change can reach, for the whole-tree source scans.

``check_32bit_size_literals.py`` and ``check_32bit_layout_offsets.py`` parse
every translation unit of a build twice with libclang. On a pull request that
is eight minutes each for a change that usually touches a handful of files,
and a unit whose own text and headers did not change cannot gain a finding.
``--changed-since <commit>`` on either tool asks this module for the units the
change can reach and scans only those; a push to master still scans all.

The answer errs towards scanning. Everything is scanned when

* git cannot list the change or the build has no dependency log (the build
  must be a Ninja build that has compiled its sources);
* a file outside ``src/`` changed that is not known to be unable to affect a
  scan (documentation, tests, other platforms' build scripts are; CMake files,
  the tools and their allow lists are not);
* a changed file under ``src/`` is in no unit's dependency list, yet some
  source names it in an ``#include`` (it may be included under a condition
  this build does not meet, such as ``__i386__``).

Otherwise the units scanned are those whose dependency list holds a changed
file, the changed units themselves, and, when any header changed, every unit
the build did not compile (their headers are not known).

    changed_units.py --build-dir build --since HEAD^1     print the units

Standard library only.
"""

import argparse
import fnmatch
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Changes outside src/ that cannot alter what a scan of this build finds.
HARMLESS = (
    "docs/*", "*.md", "tests/*", "scripts/tests/*", "scripts/runs/*", "scripts/ci/*",
    "scripts/platform/vita/*", "scripts/platform/macos/*", "scripts/platform/win32/*", "scripts/platform/mingw/*",
    ".github/workflows/*",
)
# ...except these, which decide how the scanned build is configured. The Linux
# workflow is read by section (_workflow_decides_the_scan): a change to its
# triggers or to a job that runs no scan leaves the scanned build as it was.
NOT_HARMLESS = (".github/workflows/build-kisakcod-linux.yaml",)
SCAN_WORKFLOW = ".github/workflows/build-kisakcod-linux.yaml"
# Top-level sections of the workflow that cannot change a scanned build.
QUIET_SECTIONS = ("on", "concurrency", "permissions")
SCAN_MARK = "check_32bit_"  # a job that names the scans runs them
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")


def changed_files(root, since):
    """Paths changed between a commit and the work tree, relative to the
    repository, or None when git cannot say."""
    try:
        done = subprocess.run(["git", "diff", "--name-only", "--no-renames", since], cwd=root,
                              capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    if done.returncode != 0:
        return None
    return [line for line in done.stdout.splitlines() if line]


def ninja_deps(build_dir):
    """{object: [dependency paths, absolute]} from the build's dependency log,
    or None when there is none."""
    try:
        done = subprocess.run(["ninja", "-C", str(build_dir), "-t", "deps"], capture_output=True, text=True,
                              timeout=600)
    except (OSError, subprocess.SubprocessError):
        return None
    if done.returncode != 0:
        return None
    return parse_deps(done.stdout, build_dir)


def parse_deps(text, build_dir):
    deps, current = {}, None
    for line in text.splitlines():
        if not line.strip():
            current = None
        elif not line[0].isspace():
            name, _, rest = line.partition(": #deps ")
            current = deps.setdefault(name, []) if rest and "(VALID)" in rest else None
        elif current is not None:
            current.append(str((Path(build_dir) / line.strip()).resolve()))
    return deps or None


def _scan_signature(text):
    """What of the Linux workflow decides how the scanned build is made: every
    top-level section except the triggers, concurrency and permissions (the
    top-level env among them), and each job that runs one of the scans. None
    when the file does not look like a workflow."""
    sections, order, key, in_jobs, job = {}, [], None, False, None
    for line in text.splitlines():
        if line.strip() == "" or line.lstrip().startswith("#"):
            if key is not None:
                sections[key].append(line)
            continue
        if not line[0].isspace():
            name = line.partition(":")[0].strip()
            key, in_jobs, job = (name, True, None) if name == "jobs" else (name, False, None)
            if key in sections:
                return None
            sections[key] = [line]
            order.append(key)
        elif in_jobs and line.startswith("  ") and not line.startswith("   "):
            job = "jobs/" + line.partition(":")[0].strip()
            if job in sections:
                return None
            sections[job] = [line]
            order.append(job)
            key = job
        elif key is not None:
            sections[key].append(line)
    if "jobs" not in sections:
        return None
    kept = []
    for name in order:
        body = "\n".join(sections[name])
        if name.startswith("jobs/"):
            if SCAN_MARK in body:
                kept.append(body)
        elif name != "jobs" and name not in QUIET_SECTIONS:
            kept.append(body)
    return kept


def _workflow_decides_the_scan(root, since, path):
    """True unless the change to the Linux workflow since a commit leaves
    _scan_signature as it was. True also when that cannot be told."""
    if since is None or path != SCAN_WORKFLOW:
        return True
    try:
        old = subprocess.run(["git", "show", f"{since}:{path}"], cwd=root, capture_output=True, text=True,
                             timeout=120)
        new = (Path(root) / path).read_text(encoding="utf-8", errors="replace")
    except (OSError, subprocess.SubprocessError):
        return True
    if old.returncode != 0:
        return True
    before, after = _scan_signature(old.stdout), _scan_signature(new)
    return before is None or after is None or before != after


def _harmless(path):
    if any(fnmatch.fnmatchcase(path, pattern) for pattern in NOT_HARMLESS):
        return False
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in HARMLESS)


def _named_in_an_include(name, files):
    pattern = re.compile(r'^\s*#\s*include\s*[<"][^>"]*\b' + re.escape(name) + r'[>"]', re.M)
    for file in files:
        try:
            if pattern.search(Path(file).read_text(encoding="utf-8", errors="replace")):
                return file
        except OSError:
            continue
    return None


def reachable(units, changed, deps, root=ROOT, since=None):
    """(units to scan, why). units: absolute source paths. changed: paths
    relative to root, or None. deps: parse_deps' result, or None. since: the
    commit the change is measured from, which lets a change to the Linux
    workflow that touches no scanned build count as harmless. The first value
    is None when everything must be scanned."""
    root = Path(root)
    if changed is None:
        return None, "git could not list the change"
    if deps is None:
        return None, "the build has no dependency log"
    source_root = str(root / "src") + "/"
    inside, outside = [], []
    for path in changed:
        (inside if path.startswith("src/") else outside).append(path)
    for path in outside:
        if not _harmless(path) and _workflow_decides_the_scan(root, since, path):
            return None, f"{path} changed"
    units = set(units)
    owners = {}  # dependency -> units whose object lists it
    with_deps = set()
    for listed in deps.values():
        mine = [path for path in listed if path in units]
        with_deps.update(mine)
        for path in listed:
            if path.startswith(source_root):
                owners.setdefault(path, set()).update(mine)
    universe = sorted(set(owners) | units)
    selected, header_changed = set(), False
    for path in inside:
        absolute = str((root / path).resolve())
        if not path.endswith(SOURCE_SUFFIXES):
            header_changed = True
        if absolute in owners:
            selected.update(owners[absolute])
        if absolute in units:
            selected.add(absolute)
        if absolute not in owners and absolute not in units:
            where = _named_in_an_include(Path(path).name, universe)
            if where:
                return None, (f"{path} changed; no compiled unit lists it, but "
                              f"{Path(where).relative_to(root)} names it in an #include")
    if header_changed:
        selected.update(units - with_deps)
    return selected, f"{len(inside)} changed file(s) under src/ reach {len(selected)} of {len(units)} units"


def select(units, build_dirs, since, root=ROOT):
    """What a scan given --changed-since keeps: (set of units or None, why)."""
    deps = {}
    for build_dir in build_dirs:
        found = ninja_deps(build_dir)
        if found is None:
            deps = None
            break
        deps.update(found)
    return reachable(units, changed_files(root, since), deps, root, since)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, action="append", required=True)
    parser.add_argument("--since", required=True, help="the commit the change is measured from (HEAD^1 on a merge)")
    args = parser.parse_args()
    units = set()
    for build_dir in args.build_dir:
        database = build_dir / "compile_commands.json"
        if not database.is_file():
            print(f"{database} not found", file=sys.stderr)
            return 2
        for entry in json.loads(database.read_text(encoding="utf-8")):
            path = Path(entry["directory"], entry["file"]).resolve()
            if ROOT / "src" in path.parents:
                units.add(str(path))
    selected, why = select(units, args.build_dir, args.since)
    print(why if selected is not None else f"every unit: {why}")
    for path in sorted(selected or ()):
        print(Path(path).relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
