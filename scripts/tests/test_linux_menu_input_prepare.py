#!/usr/bin/env python3
"""Asset-free exporter-schema and supported control preparation regressions."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_linux_menu_input as prep
from prepare_linux_menu import read_menu_inventory


def inventory():
    rect={'xywh':[0,0,100,20],'horz_align':0,'vert_align':0}
    window={'name':None,'rect':rect,'rect_client':copy.deepcopy(rect),'style':0,'border':0,'border_size':0,'owner_draw':0,'owner_draw_flags':0,'static_flags':0,'dynamic_flags':0,'fore_color':[1]*4,'back_color':[0]*4,'background':None}
    scripts={k:None for k in ('mouse_enter_text','mouse_exit_text','mouse_enter','mouse_exit','action','on_accept','on_focus','leave_focus')};scripts['on_key']=[]
    items=[]
    for k,index in enumerate(prep.INDICES):
        control=k%2;w=copy.deepcopy(window);w.update(style=control,static_flags=0 if control else 1048576,dynamic_flags=65540)
        hooks=copy.deepcopy(scripts)
        if control: hooks.update(action='play mouse_click;',on_focus=f'play mouse_over; setLocalVarInt ui_highlight {31 if k==1 else 32}; setLocalVarString ui_choicegroup "";',leave_focus='setLocalVarInt ui_highlight 0;')
        key='MENU_FREE_LOOK' if k==0 else 'MENU_SMOOTH_MOUSE'
        items.append(dict(index=index,window=w,text_rect=copy.deepcopy(rect),type=11 if control else 1,data_type=11 if control else 1,alignment=0,font_enum=0 if control else 1,text_align_mode=8 if control else 10,text_align_x=0,text_align_y=0,text_scale=.375,text_style=3 if control else 6,item_flags=0,text=None,localized_text=None,has_scripts=bool(control),dvar=('cl_freelook' if k==1 else 'm_filter') if control else None,dvar_test=None,enable_dvar=None,dvar_flags=0,has_focus_sound=False,expressions=[0]*8 if control else [0,2,0,0,0,0,0,0],scripts=hooks,text_expression=[] if control else [{'type':0,'op':16},{'type':1,'operand_type':2,'value':'@'+key,'localized_reference':'Synthetic label'}],visible_expression=[],material_expression=[]))
    window['name']='options_look'
    menu=dict(window=window,font=None,font_index=0,full_screen=0,has_scripts=True,expressions=[0]*3,sound=None,blur_radius=7,scripts={'on_open':''.join('close '+n+';' for n in sorted(prep.CLOSE-{'self'}))+'setLocalVarBool ui_hideBack 1;','on_close':'setLocalVarBool ui_hideBack 0;','on_esc':'close self;','on_key':[]},allowed_binding=None,focus_color=[1]*4,disable_color=[0]*4,fade_cycle=20,fade_clamp_amount_in=[1,.1,.1],visible_expression=[],items=items)
    return {'string_encoding':'engine bytes mapped to U+0000..U+00FF','menus':[menu],'localized_controls':{'EXE_YES':'Synthetic yes','EXE_NO':'Synthetic no'}}

class Preparation(unittest.TestCase):
    def test_current_schema_without_profile_deterministic_no_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);export=d/'export';export.mkdir();(export/'menu-inventory.json').write_text(json.dumps(inventory()))
            with patch.object(prep,'prepare_font_files',return_value=({'synthetic.font':b'fixture'},{})):
                prep.prepare(export,d/'one');prep.prepare(export,d/'two')
                self.assertEqual((d/'one/control.menu').read_bytes(),(d/'two/control.menu').read_bytes())
                self.assertTrue((d/'one/control.menu').read_bytes().startswith(b'KUCTRL01'))
                with self.assertRaises(ValueError):prep.prepare(export,d/'one')
            self.assertFalse((export/'selected-control-profile.json').exists())
    def test_roles_and_expressions_rejected(self):
        mutations=[lambda m:m['items'][1].update(material_expression=[{'type':0,'op':16}]),lambda m:m['items'][1]['scripts'].update(action='play mouse_over;'),lambda m:m['items'][1]['scripts'].update(on_focus='play mouse_over; setLocalVarInt ui_highlight 32; setLocalVarString ui_choicegroup "";'),lambda m:m['scripts'].update(on_esc='close options_move;'),lambda m:m['scripts'].update(on_open='close self;'),lambda m:m['items'].append(copy.deepcopy(m['items'][0])),lambda m:m['items'][0].pop('material_expression'),lambda m:m['items'][0].update(extra=True),lambda m:m['items'][0]['text_expression'][1].update(value='@OTHER'),lambda m:m['items'][1].update(dvar='other')]
        for mutate in mutations:
            data=inventory();mutate(data['menus'][0])
            with self.subTest(mutate=mutate),self.assertRaises((ValueError,KeyError)):prep.encode(data['menus'][0],data['localized_controls'])
    def test_inventory_bound_and_schema(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);p=d/'menu-inventory.json';payload=json.dumps(inventory()).encode()
            p.write_bytes(b' '*(32*1024*1024)+payload)
            self.assertEqual(len(read_menu_inventory(d)['menus']),1)
            with patch('prepare_linux_menu.MENU_INVENTORY_LIMIT',32):
                with self.assertRaises(ValueError):read_menu_inventory(d)
            for bad in ({},dict(inventory(),extra=1),dict(inventory(),string_encoding='other')):
                p.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):read_menu_inventory(d)
    def test_duplicate_menu(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);data=inventory();data['menus']*=2;(d/'menu-inventory.json').write_text(json.dumps(data))
            with self.assertRaises(ValueError):prep.prepare(d,d/'out')

if __name__=='__main__':unittest.main()
