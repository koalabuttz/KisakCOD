#!/usr/bin/env python3
"""Prefer Ubuntu's main archive over the slow hosted-runner Azure mirror.

The Azure mirror stays in the hosted runners' mirror list as the last choice.
"""
from pathlib import Path
import re
import sys


def keep_azure_as_last_resort(original: str, updated: str) -> str:
    # The main archive is the first choice, but a runner that cannot resolve
    # its name can still try the Azure mirror once every other entry failed.
    azure = re.search(
        r'^(https?://azure\.archive\.ubuntu\.com/ubuntu/?)[ \t]+priority:\d+[ \t]*$',
        original, re.MULTILINE)
    priorities = [int(n) for n in re.findall(r'priority:(\d+)', updated)]
    if not azure or not priorities:
        return updated
    if not updated.endswith('\n'):
        updated += '\n'
    return updated + f'{azure.group(1)}\tpriority:{max(priorities) + 1}\n'


def use_main_archive(apt_dir: Path) -> None:
    # Support legacy sources.list, deb822 .sources, and the mirror+file list
    # used by GitHub's Ubuntu runners. Their sources point at apt-mirrors.txt
    # rather than spelling the Azure URI in the .sources file itself.
    # Only change this exact mirror: Arm ports and third-party repos keep
    # their URIs, and suites, components, architecture and signing options stay.
    sources = [apt_dir / 'sources.list', apt_dir / 'apt-mirrors.txt']
    sources.extend(sorted((apt_dir / 'sources.list.d').glob('*.list')))
    sources.extend(sorted((apt_dir / 'sources.list.d').glob('*.sources')))
    for path in sources:
        if not path.is_file():
            continue
        original = path.read_text()
        if path.name == 'apt-mirrors.txt' and re.search(
                r'^https://archive\.ubuntu\.com/ubuntu/?[ \t]+priority:',
                original, re.MULTILINE):
            continue  # already rewritten; the Azure entry at the end is deliberate
        updated = re.sub(
            r'https?://azure\.archive\.ubuntu\.com/ubuntu(?=[/\s]|$)',
            'https://archive.ubuntu.com/ubuntu', original)
        if path.name == 'apt-mirrors.txt' and updated != original:
            updated = keep_azure_as_last_resort(original, updated)
        if updated != original:
            path.write_text(updated)
            print(f'Ubuntu main archive: {path}', flush=True)


if __name__ == '__main__':
    use_main_archive(Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/etc/apt'))
