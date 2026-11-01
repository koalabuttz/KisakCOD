"""Independent label oracle: Pillow alpha, exported metrics, explicit storage precision.

Optional real-asset helper; no proprietary data is embedded. Float32 casts model
stored engine command fields; pen accumulation and bilinear sampling use double.
"""
import io
import math
import struct
from pathlib import Path
from PIL import Image
from linux_menu_composition_reference import sampling_plan


def f32(value):
    return struct.unpack('<f',struct.pack('<f',value))[0]


def reference(bundle,menu,atlas,width,height,background=None):
    base=background or (lambda x,y:(64,128,191))
    base_w,base_h=struct.unpack_from('<HH',atlas,6)
    levels=1+int(math.log2(max(base_w,base_h)))
    decoded={}
    def alpha_level(level):
        if level not in decoded:
            w,h=base_w,base_h
            end=len(atlas)
            for _ in range(level):
                end-=max(1,(w+3)//4)*max(1,(h+3)//4)*16
                w,h=max(1,w//2),max(1,h//2)
            size=max(1,(w+3)//4)*max(1,(h+3)//4)*16
            header=[124,0x81007,h,w,size,0,0]+[0]*11
            header += [32,4,struct.unpack('<I',b'DXT5')[0],0,0,0,0,0,0x1000,0,0,0,0]
            rgba=Image.open(io.BytesIO(b'DDS '+struct.pack('<31I',*header)+atlas[end-size:end])).convert('RGBA')
            assert rgba.getextrema()[:3]==((255,255),)*3
            decoded[level]=rgba.getchannel('A'),w,h
        return decoded[level]
    result={}
    descriptions=[]
    viewport=f32(height/480)
    viewport_x=f32(min(width,f32(height*1.333333373069763))/640)
    for index in (105,111):
        prior=set(result)
        item=menu['items'][index]
        text=item['text_expression'][1]['localized_reference']
        requested=f32(item['text_scale'])
        rect=item['window']['rect']['xywh']
        factor=f32(viewport*requested)
        tint=[math.floor(c*255+.5) for c in item['window']['fore_color']]
        # Legacy RGB565 uses 32/64 denominators. Pillow supplies independent alpha.
        white=tuple(math.floor(c*255+.5) for c in (31/32,63/64,31/32))
        role='smallFont' if factor<=.25 else 'textFont' if factor<.4 else 'bigFont' if factor<.55 else 'extraBigFont'
        font=(Path(bundle)/(role+'.font')).read_bytes()
        _,font_height,_,_=struct.unpack_from('<4I',font)
        normalized=f32(requested*48/font_height)
        scale_x,scale_y=f32(normalized*viewport_x),f32(normalized*viewport)
        glyphs={chr(i+32):struct.unpack_from('<HbbBBBx4f',font,16+i*24) for i in range(96)}
        textwidth=int(sum(glyphs[c][3] for c in text)*normalized)
        textheight=int(font_height*normalized)
        virtual_x=f32(rect[0]+item['text_align_x']+rect[2]-textwidth)
        virtual_y=f32(rect[1]+item['text_align_y']+(rect[3]+textheight)*.5)
        command_x=math.floor(f32(f32(width/2+f32(virtual_x*viewport_x))+.5))
        command_y=math.floor(f32(f32(virtual_y*viewport)+.5))
        x,y=command_x-.5*scale_x,command_y-.5*scale_y
        used_levels=set()
        used_taps=set()
        description={'source_index':index,'font_role':role,'physical_scale_x':scale_x,
                     'physical_scale_y':scale_y,'baseline':[x,y]}
        descriptions.append(description)
        def draw(g,pen_x,pen_y,shadow):
            _,x0,y0,_,pw,ph,s0,t0,s1,t1=g
            gx,gy=f32(pen_x+x0*scale_x),f32(pen_y+y0*scale_y)
            gw,gh=f32(pw*scale_x),f32(ph*scale_y)
            if not gw or not gh:return
            if shadow:gx,gy=f32(gx+2),f32(gy+2)
            level,offsets=sampling_plan(abs(s1-s0)*base_w/gw,abs(t1-t0)*base_h/gh,base_w,base_h,levels)
            used_levels.add(level);used_taps.add(len(offsets))
            alpha,w,h=alpha_level(level)
            for py in range(max(0,math.ceil(gy-.5)),min(height,math.ceil(gy+gh-.5))):
                for px in range(max(0,math.ceil(gx-.5)),min(width,math.ceil(gx+gw-.5))):
                    u=s0+(px+.5-gx)/gw*(s1-s0)
                    v=t0+(py+.5-gy)/gh*(t1-t0)
                    samples=[]
                    for du,dv in offsets:
                        tx=max(0,min(w-1,(u+du)*w-.5));ty=max(0,min(h-1,(v+dv)*h-.5))
                        ix,iy=int(tx),int(ty);fx,fy=tx-ix,ty-iy
                        samples.append(sum(alpha.getpixel((min(ix+ox,w-1),min(iy+oy,h-1)))*wx*wy
                                           for ox,wx in ((0,1-fx),(1,fx)) for oy,wy in ((0,1-fy),(1,fy))))
                    a=sum(samples)/len(samples)/255*tint[3]/255
                    old=result.get((px,py),base(px,py))
                    color=(0,0,0) if shadow else tuple(c*t/255 for c,t in zip(white,tint))
                    result[px,py]=tuple(math.floor(src*a+dst*(1-a)+.5) for src,dst in zip(color,old))
        for character in text:
            draw(glyphs[character],x,y,True)
            draw(glyphs[character],x,y,False)
            x+=glyphs[character][3]*scale_x
        description.update(mip_levels=sorted(used_levels),filter_taps=sorted(used_taps))
        visible=[point for point in result if point not in prior and max(abs(a-b) for a,b in zip(result[point],base(*point)))>15]
        assert len(visible)>=8,(index,width,height,'label oracle has no meaningful visible pixels')
    changed=[(point,color) for point,color in result.items() if max(abs(a-b) for a,b in zip(color,base(*point)))>15]
    positions=[(f'label{i}',*point) for i,(point,_) in enumerate(changed[::max(1,len(changed)//100)])]
    positions.append(('background',5,5));result[5,5]=base(5,5)
    return result,positions,descriptions
