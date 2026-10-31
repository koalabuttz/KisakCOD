#!/usr/bin/env python3
"""Synthetic composition metadata checks; no proprietary image or menu bytes."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from prepare_linux_menu_composition import encode_composition

def fixture():
    items=[]
    for n in range(8):
        w=dict(style=3,border=0,owner_draw=0,owner_draw_flags=0,background='white',rect=dict(xywh=[0,0,100,50],horz_align=0,vert_align=0),border_size=1,static_flags=0,dynamic_flags=4,fore_color=[1,1,1,1],back_color=[0,0,0,0])
        items.append(dict(index=n,type=0,text=None,has_scripts=False,dvar=None,dvar_test=None,enable_dvar=None,dvar_flags=0,has_focus_sound=False,item_flags=0,window=w,expressions=[0]*8,material_expression=[]))
    items[0]['window']['background']=None;items[0]['expressions'][2]=2
    items[0]['material_expression']=[dict(type=0,op=16),dict(type=1,operand_type=2,value='white',localized_reference=None)]
    return dict(window=dict(name='options_look'),items=items,fade_cycle=1,fade_clamp_amount_in=[1,.1,0])
class Tests(unittest.TestCase):
    def test_valid_literal_backing_and_order(self):
        data,names=encode_composition(fixture());self.assertEqual(data[:8],b'KUCOMP01');self.assertEqual(names,{'white','ui_cursor'})
    def test_reject_dynamic_or_unavailable_background(self):
        for field,value in [('material_expression',[]),('expressions',[0]*8),('has_scripts',True),('dvar','arbitrary')]:
            doc=fixture();doc['items'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):encode_composition(doc)
        doc=fixture();doc['items'][0]['material_expression'][1]['value']='unknown'
        with self.assertRaises(ValueError):encode_composition(doc)
    def test_reject_order_geometry_and_style(self):
        for field,value in [('index',9),('type',11)]:
            doc=fixture();doc['items'][2][field]=value
            with self.assertRaises(ValueError):encode_composition(doc)
        for field,value in [('style',5),('border',1),('owner_draw',1),('background','unknown')]:
            doc=fixture();doc['items'][2]['window'][field]=value
            with self.assertRaises(ValueError):encode_composition(doc)
        doc=fixture();doc['items'][2]['window']['rect']['xywh'][0]=float('nan')
        with self.assertRaises(ValueError):encode_composition(doc)
    def test_mirrored_rectangles_preserved(self):
        doc=fixture();doc['items'][2]['window']['rect']['xywh'][3]=-64
        self.assertNotEqual(encode_composition(doc)[0],encode_composition(fixture())[0])
if __name__=='__main__':unittest.main()
