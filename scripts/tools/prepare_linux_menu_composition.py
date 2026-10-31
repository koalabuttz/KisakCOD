#!/usr/bin/env python3
"""Prepare genuine options_look backing/decorations/cursor from local DB exports."""
import argparse
import json
import math
from pathlib import Path
import struct
from ui_material_metadata import encode_material

def encode_composition(menu):
    def word(v):
        if type(v) is not int or not 0 <= v <= 0xffffffff: raise ValueError('invalid composition word')
        return struct.pack('<I',v)
    def scalar(v):
        if not isinstance(v,(int,float)) or not math.isfinite(v): raise ValueError('invalid composition scalar')
        return struct.pack('<f',v)
    def name(v):
        if not isinstance(v,str) or not v or len(v)>255 or v in ('.','..') or any(ord(c)<32 or ord(c)>=127 or c in '/\\' for c in v):raise ValueError('invalid composition name')
        return word(len(v))+v.encode('ascii')
    if menu['window']['name']!='options_look' or len(menu['items'])<8:raise ValueError('unsupported composition menu')
    result=b'KUCOMP01'+word(8)+word(menu['fade_cycle'])+b''.join(scalar(v) for v in menu['fade_clamp_amount_in'])
    names=set()
    for index,item in enumerate(menu['items'][:8]):
        w=item['window']
        if item['index']!=index or item['type']!=0 or item['text'] or item['has_scripts'] or item['dvar'] or item['dvar_test'] or item['enable_dvar'] or item['dvar_flags'] or item['has_focus_sound'] or item['item_flags']:raise ValueError('unsupported composition item behavior')
        if w['style']!=3 or w['border'] or w['owner_draw'] or w['owner_draw_flags']:raise ValueError('unsupported composition window')
        material=w['background']
        if index==0:
            expected=[{'type':0,'op':16},{'type':1,'operand_type':2,'value':'white','localized_reference':None}]
            if item['material_expression']!=expected or item['expressions']!=[0,0,2,0,0,0,0,0] or material is not None:raise ValueError('unsupported backing material expression')
            material='white'
        elif any(item['expressions']) or item['material_expression']:raise ValueError('dynamic decoration unsupported')
        if material not in ('white','button_highlight_end'):raise ValueError('unsupported decoration material')
        names.add(material)
        result+=word(index)+b''.join(scalar(v) for v in w['rect']['xywh'])
        result+=word(w['rect']['horz_align'])+word(w['rect']['vert_align'])+word(w['style'])+word(w['border'])+scalar(w['border_size'])+word(w['static_flags'])+word(w['dynamic_flags'])
        result+=b''.join(scalar(v) for field in ('fore_color','back_color') for v in w[field])+name(material)
    return result+name('ui_cursor'),names|{'ui_cursor'}

def prepare(export,bundle):
    def read(name):
        path=export/name
        if path.stat().st_size>128*1024*1024:raise ValueError('inventory exceeds bound')
        return json.loads(path.read_text())
    menus=[m for m in read('menu-inventory.json')['menus'] if m['window']['name']=='options_look']
    if len(menus)!=1:raise ValueError('expected one options_look')
    data,names=encode_composition(menus[0]);materials=read('material-inventory.json')['materials'];files={'composition.bin':data}
    cursor=read('cursor-inventory.json')
    if cursor!={'registration_name':'ui_cursor','available':True}:raise ValueError('genuine cursor unavailable')
    for name in sorted(names):
        found=[m for m in materials if m['name']==name]
        if len(found)!=1:raise ValueError('missing or duplicate composition material')
        files[name+'.material']=encode_material(found[0])
    if not bundle.is_dir():raise ValueError('existing private bundle required')
    if any((bundle/name).exists() for name in files):raise ValueError('refusing to overwrite composition files')
    written=[]
    try:
        for name,data in files.items():
            path=bundle/name
            with path.open('xb') as stream:written.append(path);path.chmod(0o600);stream.write(data)
    except BaseException:
        for path in written:path.unlink()
        raise
    return sorted(files)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--export',required=True,type=Path);p.add_argument('--bundle',required=True,type=Path);a=p.parse_args();print('prepared',prepare(a.export,a.bundle))
