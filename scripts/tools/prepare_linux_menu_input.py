#!/usr/bin/env python3
"""Prepare the bounded options_look control profile from authorized local exports.
Copies metadata and font roles only; never executes menu scripts.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
from prepare_linux_menu import read_json, prepare_font_files

INDICES=(105,106,111,112)
CLOSE={'self','options_move','options_shoot','options_misc','options_graphics','options_sound','options_game','options_view','options_defaults','options_driverinfo','options_credits','options_multi','options_graphics_defaults','options_graphics_texture','options_control_defaults'}

def require(ok, message):
    if not ok: raise ValueError(message)

def text(s):
    s='' if s is None else s
    require(isinstance(s,str),'invalid string')
    b=s.encode('ascii'); require(len(b)<=4096 and all(32<=c<127 for c in b),'unsupported string encoding/length')
    return struct.pack('<I',len(b))+b

def word(*values):
    require(all(type(v) is int and 0<=v<=0xffffffff for v in values),'invalid integer')
    return struct.pack('<'+'I'*len(values),*values)

def real(*values):
    require(all(isinstance(v,(float,int)) and math.isfinite(v) for v in values),'nonfinite scalar')
    return struct.pack('<'+'f'*len(values),*values)

def script(s):
    s=s or '';text(s)
    tokens=[];p=0
    while p<len(s):
        if s[p]==' ': p+=1;continue
        if s[p]==';': tokens.append(';');p+=1;continue
        match=re.match(r'"([^"\\]*)"|([A-Za-z0-9_]+)',s[p:])
        require(match is not None,'unsupported script syntax')
        tokens.append(match.group(1) if match.group(1) is not None else match.group(2));p+=match.end()
        require(p==len(s) or s[p] in ' ;','invalid script separator')
    command=[]
    for token in tokens+[';']:
        if token!=';': command.append(token);continue
        if command:
            c=command
            ok=(len(c)==2 and ((c[0]=='close' and c[1] in CLOSE) or (c[0]=='play' and c[1] in ('mouse_click','mouse_over')))) or (len(c)==3 and ((c[:2]==['setLocalVarBool','ui_hideBack'] and c[2] in ('0','1')) or (c[:2]==['setLocalVarInt','ui_highlight'] and c[2] in ('0','31','32')) or c==['setLocalVarString','ui_choicegroup','']))
            require(ok,'unsupported script command')
        command=[]
    return text(s)

def rect(r):
    require(set(r)=={'xywh','horz_align','vert_align'} and len(r['xywh'])==4,'unsupported rect fields')
    x,y,w,h=r['xywh'];require(abs(x)<=16384 and abs(y)<=16384 and 0<=w<=16384 and 0<=h<=16384 and 0<=r['horz_align']<=7 and 0<=r['vert_align']<=7,'invalid rect')
    return real(x,y,w,h)+word(r['horz_align'],r['vert_align'])

def color(c):
    require(len(c)==4 and all(0<=v<=1 for v in c),'invalid color');return real(*c)

def window(w):
    require(set(w)=={'name','rect','rect_client','style','border','border_size','owner_draw','owner_draw_flags','static_flags','dynamic_flags','fore_color','back_color','background'},'unsupported window fields')
    require(not w['background'] and not w['owner_draw'] and not w['owner_draw_flags'] and not w['border'] and w['style'] in (0,1) and 0<=w['border_size']<=32,'unsupported window behavior')
    return rect(w['rect'])+rect(w['rect_client'])+word(w['style'],w['border'])+real(w['border_size'])+word(w['owner_draw'],w['owner_draw_flags'],w['static_flags'],w['dynamic_flags'])+color(w['fore_color'])+color(w['back_color'])

def encode(menu, profile, controls):
    require(menu['window']['name']=='options_look','wrong menu')
    require(set(menu)=={'window','font','font_index','full_screen','has_scripts','expressions','sound','blur_radius','scripts','allowed_binding','focus_color','disable_color','fade_cycle','fade_clamp_amount_in','visible_expression','items'},'unsupported parent fields')
    require(all(menu[k]==v for k,v in profile['menu'].items()) and controls==profile['controls'],'profile differs from exported menu')
    require(menu['has_scripts']==bool(any(menu['scripts'].values())),'parent script presence mismatch')
    require(not menu['font'] and not menu['font_index'] and not menu['full_screen'] and not any(menu['expressions']) and not menu['sound'] and not menu['allowed_binding'] and not menu['visible_expression'],'unsupported parent behavior')
    require(not menu['window']['style'] and not menu['window']['static_flags'] and not menu['window']['dynamic_flags'],'unsupported parent flags')
    require(set(menu['scripts'])=={'on_open','on_close','on_esc','on_key'} and not menu['scripts']['on_key'],'unsupported menu key scripts')
    require(0<=menu['fade_cycle']<=1000 and len(menu['fade_clamp_amount_in'])==3 and all(0<=v<=1 for v in menu['fade_clamp_amount_in']) and 0<=menu['blur_radius']<=32,'invalid fade/blur')
    items=[]
    for n in INDICES:
        found=[i for i in menu['items'] if i['index']==n];require(len(found)==1,'missing/duplicate selected item');items+=found
    require(len(profile['items'])==4 and all(all(i[k]==v for k,v in p.items()) for i,p in zip(items,profile['items'])),'selected profile mismatch')
    b=b'KUCTRL01'+word(4)+text('options_look')+window(menu['window'])+word(menu['font_index'],menu['full_screen'],menu['fade_cycle'])+real(*menu['fade_clamp_amount_in'],menu['blur_radius'])+color(menu['focus_color'])+color(menu['disable_color'])
    b+=b''.join(script(menu['scripts'][k]) for k in ('on_open','on_close','on_esc'))
    translations=dict(controls)
    for i,key in ((items[0],'MENU_FREE_LOOK'),(items[2],'MENU_SMOOTH_MOUSE')):
        e=i['text_expression'];require(len(e)==2 and e[0]=={'type':0,'op':16} and set(e[1])=={'type','operand_type','value','localized_reference'} and e[1]['type']==1 and e[1]['operand_type']==2 and e[1]['value']=='@'+key,'unsupported expression');translations[key]=e[1]['localized_reference']
    for key in ('EXE_YES','EXE_NO','MENU_FREE_LOOK','MENU_SMOOTH_MOUSE'):
        value=translations[key];require(isinstance(value,str) and 0<len(value)<=255 and '^' not in value,'invalid localization');b+=text(key)+text(value)
    for k,i in enumerate(items):
        control=k%2
        require(set(i)=={'index','window','text_rect','type','data_type','alignment','font_enum','text_align_mode','text_align_x','text_align_y','text_scale','text_style','item_flags','text','localized_text','has_scripts','dvar','dvar_test','enable_dvar','dvar_flags','has_focus_sound','expressions','scripts','text_expression','visible_expression'},'unsupported item fields')
        require(not i['window']['name'] and not i['text'] and not i['localized_text'] and not i['dvar_test'] and not i['enable_dvar'] and not i['dvar_flags'] and not i['has_focus_sound'] and not i['visible_expression'],'unsupported item behavior')
        require(i['type']==i['data_type']==(11 if control else 1) and i['alignment']==0 and i['font_enum']==(0 if control else 1) and i['text_align_mode']==(8 if control else 10) and i['text_style']==(3 if control else 6) and not i['item_flags'] and i['window']['style']==control and i['window']['static_flags']==(0 if control else 1048576) and i['window']['dynamic_flags']==65540 and 0<i['text_scale']<=4 and abs(i['text_align_x'])<=16384 and abs(i['text_align_y'])<=16384,'unsupported control profile')
        require(i['dvar']==(('cl_freelook' if k==1 else 'm_filter') if control else None) and i['expressions']==([0]*8 if control else [0,2,0,0,0,0,0,0]) and (not control or not i['text_expression']),'unsupported dvar/expression')
        scripts=i['scripts'];require(i['has_scripts']==bool(any(scripts.values())),'item script presence mismatch');require(set(scripts)=={'mouse_enter_text','mouse_exit_text','mouse_enter','mouse_exit','action','on_accept','on_focus','leave_focus','on_key'} and all(not scripts[n] for n in ('mouse_enter_text','mouse_exit_text','mouse_enter','mouse_exit','on_accept','on_key')) and (control or not any(scripts.values())),'unsupported item script hooks')
        b+=word(i['index'])+window(i['window'])+rect(i['text_rect'])+word(i['type'],i['data_type'],i['alignment'],i['font_enum'],i['text_align_mode'])+real(i['text_align_x'],i['text_align_y'],i['text_scale'])+word(i['text_style'],i['item_flags'],i['dvar_flags'])+text(i['dvar'])
        b+=b''.join(script(scripts[n]) for n in ('action','on_focus','leave_focus'))+word(0 if control else 2)
        if not control: b+=word(0,16,1,2)+text(i['text_expression'][1]['value'])
    require(len(b)<=32768,'control metadata exceeds bound');return b

def prepare(export,output):
    inventory=read_json(export/'menu-inventory.json',32*1024*1024);profile=read_json(export/'selected-control-profile.json')
    menus=[m for m in inventory['menus'] if m['window']['name']=='options_look'];require(len(menus)==1,'menu not unique')
    payload=encode(menus[0],profile,inventory['localized_controls']);files,roles=prepare_font_files(export);files['control.menu']=payload
    require(not output.exists(),'output must be new private directory');output.mkdir(mode=0o700)
    for name,data in files.items(): (output/name).write_bytes(data)
    (output/'provenance.json').write_text(json.dumps({'source':str(export),'menu':'options_look','items':INDICES,'font_roles':roles,'sha256':{k:hashlib.sha256(v).hexdigest() for k,v in files.items()},'scope':'Genuine selected control metadata; no scripts executed during preparation; parent blur preserved but item-only painter does not render blur'},indent=2)+'\n')
    print(f'control_bundle=prepared bytes={len(payload)} path={output}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--export',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try: prepare(a.export,a.output)
    except (ValueError,KeyError,OSError,UnicodeError,TypeError,struct.error) as e: p.exit(1,f'prepare controls: {e}\n')
