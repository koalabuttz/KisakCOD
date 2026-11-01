#!/usr/bin/env python3
"""Select genuine local menu/material metadata for the independent pixel oracle.

No assets or authored reference profiles are embedded. The optional cursor_image
field in older hand-selected files was diagnostic only and is not needed by the
oracle; this output contains no machine-specific image path.
"""
import argparse
import json
from pathlib import Path
from prepare_linux_menu import read_menu_inventory, read_json
from prepare_linux_menu_composition import encode_composition
from ui_material_metadata import encode_material


def select_reference(inventory, material_inventory, cursor):
    menus=[m for m in inventory['menus'] if m['window']['name']=='options_look']
    if len(menus)!=1: raise ValueError('expected exactly one options_look menu')
    menu=menus[0]
    _,names=encode_composition(menu)
    if cursor!={'registration_name':'ui_cursor','available':True}:
        raise ValueError('genuine cursor is unavailable')
    materials=[]
    for name in sorted(names):
        found=[m for m in material_inventory['materials'] if m['name']==name]
        if len(found)!=1: raise ValueError('missing or duplicate material: '+name)
        material=found[0]
        encode_material(material)  # Validate complete source description, not a guessed binding.
        if len(material['textures'])!=1:
            raise ValueError('oracle requires the admitted single image binding')
        image=material['textures'][0]['image']
        if not isinstance(image,str) or not image or image in ('.','..') or any(c in image for c in '/\\'):
            raise ValueError('unsafe image identifier')
        materials.append(material)
    return dict(menu_window=menu['window'],fade_cycle=menu['fade_cycle'],
                fade_clamp_amount_in=menu['fade_clamp_amount_in'],
                items=menu['items'][:8],materials=materials)


def prepare(export, output):
    result=select_reference(read_menu_inventory(export),
                            read_json(export/'material-inventory.json',8*1024*1024),
                            read_json(export/'cursor-inventory.json',4096))
    payload=(json.dumps(result,indent=2)+'\n').encode('utf-8')
    # Validate/serialize fully before publishing. Existing files are never replaced.
    created=False
    try:
        with output.open('xb') as stream:
            created=True;output.chmod(0o600);stream.write(payload)
    except BaseException:
        if created: output.unlink()
        raise
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    prepare(args.export,args.output)
    print('composition_reference=prepared path='+str(args.output))
