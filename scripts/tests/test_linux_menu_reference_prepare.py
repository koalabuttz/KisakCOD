#!/usr/bin/env python3
"""Synthetic oracle selection regression; no game metadata/image payloads."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from prepare_linux_menu_reference import select_reference,prepare
from test_linux_menu_composition_prepare import fixture


def material(name):
    shader=dict(name='synthetic',word_count=1,bytecode_available=True,fnv1a64='0000000000000000')
    draw=dict(vertex_shader=shader,pixel_shader=shader,custom_sampler_flags=0,per_prim_count=0,per_obj_count=0,stable_count=1,arguments=[dict(type=2,dest=0,value=42)])
    tech=dict(set_name='synthetic',unlit=dict(name='synthetic',flags=0,passes=[draw]))
    return dict(name=name,state_flags=0,camera_region=0,game_flags=0,sort_key=0,constants=[],textures=[dict(name_hash=42,image='synthetic_image',semantic=0,sampler_state=226)],state_bits_entry_unlit=0,state_bits=[[0,0]],technique=tech,remapped_technique=copy.deepcopy(tech))


def data():
    menu=fixture();menu['items'][2]['window']['background']='button_highlight_end'
    return dict(menus=[menu],string_encoding="engine bytes mapped to U+0000..U+00FF",localized_controls={}),dict(materials=[material(n) for n in ('white','button_highlight_end','ui_cursor')]),dict(registration_name='ui_cursor',available=True)


class Tests(unittest.TestCase):
    def test_selects_exact_order_and_preserves_descriptions(self):
        inv,mats,cursor=data();out=select_reference(inv,mats,cursor)
        self.assertEqual(out['items'],inv['menus'][0]['items'][:8])
        self.assertEqual({m['name']:m for m in out['materials']},{m['name']:m for m in mats['materials']})
        self.assertNotIn('cursor_image',out)
    def test_rejects_ambiguous_or_missing_sources(self):
        for mutate in (lambda i,m,c:i['menus'].append(copy.deepcopy(i['menus'][0])),lambda i,m,c:m['materials'].pop(),lambda i,m,c:m['materials'].append(copy.deepcopy(m['materials'][0])),lambda i,m,c:c.update(available=False),lambda i,m,c:i['menus'][0]['items'][0]['material_expression'].clear(),lambda i,m,c:m['materials'][0]['textures'][0].update(image='../bad')):
            inv,mats,cursor=data();mutate(inv,mats,cursor)
            with self.assertRaises(ValueError):select_reference(inv,mats,cursor)
    def test_deterministic_private_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);inv,mats,cursor=data()
            for name,value in [('menu-inventory.json',inv),('material-inventory.json',mats),('cursor-inventory.json',cursor)]: (root/name).write_text(json.dumps(value))
            one=root/'one.json';two=root/'two.json';prepare(root,one);prepare(root,two)
            self.assertEqual(one.read_bytes(),two.read_bytes());self.assertEqual(one.stat().st_mode&0o777,0o600)
            before=one.read_bytes()
            with self.assertRaises(FileExistsError):prepare(root,one)
            self.assertEqual(one.read_bytes(),before)
if __name__=='__main__':unittest.main()
