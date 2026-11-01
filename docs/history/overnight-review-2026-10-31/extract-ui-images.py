#!/usr/bin/env python3
"""Copy only the five admitted UI IWI images from an authorized local installation.

No download or decoder. Native loaders validate actual image format/mip content.
Identical duplicate entries are accepted; conflicting duplicates abort before any
output is created. A new private output directory is required.
"""
import argparse
import hashlib
from pathlib import Path
import zipfile

NAMES=('gamefonts_pc','devfonts','qerfont','3_cursor3','button_highlight_end')
MAX_BYTES=32*1024*1024

def extract(game, output):
    if output.exists():raise ValueError('output directory must not exist')
    wanted={'images/'+name+'.iwi':name+'.iwi' for name in NAMES}
    found={};origins={}
    archives=sorted((game/'main').glob('*.iwd'))
    if not archives:raise ValueError('no main/*.iwd archives found')
    for archive in archives:
        with zipfile.ZipFile(archive) as source:
            for info in source.infolist():
                if info.filename not in wanted:continue
                if not 28<=info.file_size<=MAX_BYTES:raise ValueError('IWI size outside bound: '+info.filename)
                with source.open(info) as stream:payload=stream.read(MAX_BYTES+1)
                if len(payload)!=info.file_size or payload[:4]!=b'IWi\x06':raise ValueError('invalid IWI bytes: '+info.filename)
                name=wanted[info.filename]
                if name in found and found[name]!=payload:
                    raise ValueError('conflicting duplicate '+name+' in '+str(archive)+' and '+origins[name])
                found[name]=payload;origins.setdefault(name,str(archive))
    missing=sorted(set(wanted.values())-found.keys())
    if missing:raise ValueError('missing required images: '+', '.join(missing))
    output.mkdir(mode=0o700)
    written=[]
    try:
        for name in sorted(found):
            path=output/name
            with path.open('xb') as stream:
                written.append(path);path.chmod(0o600);stream.write(found[name])
    except BaseException:
        for path in written:path.unlink()
        output.rmdir()
        raise
    return {name:hashlib.sha256(data).hexdigest() for name,data in sorted(found.items())}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    for name,digest in extract(args.game_dir,args.output).items():print(digest+'  '+name)
