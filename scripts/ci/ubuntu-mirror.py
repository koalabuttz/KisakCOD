#!/usr/bin/env python3
"""Use Ubuntu's main archive instead of the slow hosted-runner Azure mirror."""
from pathlib import Path
import re
import sys


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
        updated = re.sub(
            r'https?://azure\.archive\.ubuntu\.com/ubuntu(?=[/\s]|$)',
            'https://archive.ubuntu.com/ubuntu', original)
        if updated != original:
            path.write_text(updated)
            print(f'Ubuntu main archive: {path}', flush=True)


if __name__ == '__main__':
    use_main_archive(Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/etc/apt'))
