#!/usr/bin/env python3
"""Prepare one genuine static auto-wrapped text item from local offline DB exports.
No assets are downloaded; output belongs outside the repository.
"""
import argparse
import json
import math
from pathlib import Path
import struct
from ui_material_metadata import encode_material

def bounded_read(path, maximum):
    with path.open('rb') as stream:
        data=stream.read(maximum+1)
    if len(data)>maximum: raise ValueError('input exceeds supported size: '+str(path))
    return data

def read_json(path, maximum=2*1024*1024):
    return json.loads(bounded_read(path,maximum))

ROLES = ('smallFont', 'textFont', 'bigFont', 'extraBigFont', 'boldFont', 'consoleFont', 'objectiveFont')

def encode_item(menu, item):
    window = item['window']
    if item['has_scripts'] or any(item['expressions']) or item['dvar'] or item['dvar_test'] or item['enable_dvar'] or item['dvar_flags'] or item['has_focus_sound']:
        raise ValueError('item scripts, expressions, dvars or sound are unsupported')
    if item['type'] != 0 or item['item_flags'] or window['owner_draw'] or window['owner_draw_flags'] or window['background'] or window['style']:
        raise ValueError('only static text items without ownerdraw/background are supported')
    if not window['static_flags'] & 0x800000 or item['text_style'] not in (0, 3):
        raise ValueError('item is not a supported auto-wrapped text style')
    if not isinstance(item['text'], str):
        raise ValueError('item has no static text')
    text = item['localized_text'] if item['text'].startswith('@') else item['text']
    if not isinstance(text, str):
        raise ValueError('genuine localized text is unavailable')
    data = text.encode('ascii')
    if len(data) > 1023 or any((c < 32 and c not in (10, 13)) or c >= 127 for c in data) or any('^' + c in text for c in '0123456789'):
        raise ValueError('unsupported item text')
    source = f"{menu['window']['name']}:{item['index']}".encode('ascii')
    if not source or len(source)>255 or any(c<32 or c>=127 for c in source):
        raise ValueError('unsupported source identifier')
    rect = window['rect']
    # UI_Init defaults, explicitly not a claim that offline UI dvars were initialized.
    defaults = (.25, .4, .55)
    floats = (*rect['xywh'], window['border_size'], item['text_align_x'], item['text_align_y'], item['text_scale'], *window['fore_color'], *defaults)
    if len(floats)!=15 or not all(math.isfinite(x) for x in floats):
        raise ValueError('invalid menu scalar fields')
    words = (rect['horz_align'], rect['vert_align'], window['border'], item['text_align_mode'], item['text_style'], item['font_enum'], item['item_flags'], window['static_flags'], window['dynamic_flags'], len(data), len(source))
    if any(type(x) is not int or not 0<=x<=0xffffffff for x in words):
        raise ValueError('invalid menu integer fields')
    return b'KUMENU01' + struct.pack('<15f11I', *floats, *words) + data + source

def prepare(export, destination, menu_name, item_index):
    inventory=read_json(export/'menu-inventory.json',32*1024*1024)
    menus=[m for m in inventory['menus'] if m['window']['name']==menu_name]
    if len(menus)!=1: raise ValueError('menu name must identify exactly one exported menu')
    menu=menus[0];items=[i for i in menu['items'] if i['index']==item_index]
    if len(items)!=1: raise ValueError('item index unavailable')
    payload=encode_item(menu,items[0])
    roles=read_json(export/'font-roles.json')
    manifest=read_json(export/'manifest.json')
    files={'item.menu':payload}
    for role in ROLES:
        matches=[x for x in roles['roles'] if x['role']==role]
        if len(matches)!=1 or not matches[0]['actual_asset_name']: raise ValueError('missing genuine font role '+role)
        fonts=[x for x in manifest['fonts'] if x['name']==matches[0]['actual_asset_name']]
        if len(fonts)!=1: raise ValueError('font manifest role mismatch')
        font=fonts[0];filename=font['file']
        if Path(filename).name!=filename or not filename.endswith('.font'): raise ValueError('invalid export font filename')
        files[role+'.font']=bounded_read(export/filename,128*1024)
        files[role+'.material']=encode_material(font['material'])
    if destination.exists(): raise ValueError('output must be a new private directory')
    destination.mkdir(mode=0o700,parents=False)
    for name,data in files.items(): (destination/name).write_bytes(data)
    (destination/'provenance.json').write_text(json.dumps({'menu':menu_name,'item':item_index,'source':str(export),'font_roles':roles,'thresholds_source':'UI_Init defaults: .25/.4/.55; offline UI runtime not initialized','scope':'selected static text painter only; menu visibility/scripts not executed'},indent=2)+'\n')
    print(f'menu_bundle=prepared item={menu_name}:{item_index} path={destination}; native resolver validates material support at load')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--export',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--menu',required=True);p.add_argument('--item',type=int,required=True);a=p.parse_args()
    try: prepare(a.export,a.output,a.menu,a.item)
    except (ValueError,KeyError,OSError,UnicodeError,TypeError,struct.error) as e: p.exit(1,f'prepare menu: {e}\n')
