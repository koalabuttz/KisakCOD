"""Independent actual label oracle: Pillow DXT alpha, exported fields/metrics, Python layout.
Optional real-asset test helper; requires Pillow. Private assets are caller inputs.
"""
import io,json,math,struct
from pathlib import Path
from PIL import Image
def reference(bundle,menu,atlas,width,height):
 IWI=atlas
 result={};descriptions=[]
 for index in (105,111):
  prior=set(result)
  item=menu['items'][index];text=item['text_expression'][1]['localized_reference'];viewport=height/480;requested=item['text_scale'];r=item['window']['rect']['xywh'];factor=viewport*requested
  tint=[math.floor(c*255+.5) for c in item['window']['fore_color']]
  # Legacy Image_DecompressDxt1 expands RGB565 by 32/64 rather than 31/63.
  # Pillow supplies independent alpha; retain the engine's documented RGB
  # convention and derive vertex tint from exported fields, not screenshots.
  white=tuple(math.floor(c*255+.5) for c in (31/32,63/64,31/32))
  role='smallFont' if factor<=.25 else 'textFont' if factor<.4 else 'bigFont' if factor<.55 else 'extraBigFont'
  font=(Path(bundle)/(role+'.font')).read_bytes();_,ph,count,_=struct.unpack_from('<4I',font);normalized=struct.unpack('<f',struct.pack('<f',requested*48/ph))[0];scale=normalized*viewport
  glyphs={chr(i+32):struct.unpack_from('<HbbBBBx4f',font,16+i*24) for i in range(96)}
  level=max(0,math.floor(math.log2(1/scale)+.5));w,h=struct.unpack_from('<HH',IWI,6);end=len(IWI)
  for _ in range(level):
   end-=max(1,(w+3)//4)*max(1,(h+3)//4)*16;w=max(1,w//2);h=max(1,h//2)
  size=max(1,(w+3)//4)*max(1,(h+3)//4)*16
  hdr=[124,0x81007,h,w,size,0,0]+[0]*11+[32,4,struct.unpack('<I',b'DXT5')[0],0,0,0,0,0,0x1000,0,0,0,0]
  rgba=Image.open(io.BytesIO(b'DDS '+struct.pack('<31I',*hdr)+IWI[end-size:end])).convert('RGBA');alpha=rgba.getchannel('A')
  assert rgba.getextrema()[:3]==((255,255),)*3
  textwidth=int(sum(glyphs[c][3] for c in text)*normalized);textheight=int(ph*normalized)
  x=math.floor(width/2+(r[0]+item['text_align_x']+r[2]-textwidth)*viewport+.5)-.5*scale
  y=math.floor((r[1]+item['text_align_y']+(r[3]+textheight)*.5)*viewport+.5)-.5*scale
  descriptions.append({'source_index':index,'font_role':role,'mip_level':level,'physical_scale':scale,'baseline':[x,y]})
  def draw(g,x,y,shadow):
   _,x0,y0,dx,pw,ph,s0,t0,s1,t1=g;gx=x+x0*scale+(2 if shadow else 0);gy=y+y0*scale+(2 if shadow else 0);gw=pw*scale;gh=ph*scale
   if not gw or not gh:return
   for py in range(max(0,math.ceil(gy-.5)),min(height,math.ceil(gy+gh-.5))):
    for px in range(max(0,math.ceil(gx-.5)),min(width,math.ceil(gx+gw-.5))):
     tx=max(0,min(w-1,(s0+(px+.5-gx)/gw*(s1-s0))*w-.5));ty=max(0,min(h-1,(t0+(py+.5-gy)/gh*(t1-t0))*h-.5));ix=int(tx);iy=int(ty);fx=tx-ix;fy=ty-iy
     a=sum(alpha.getpixel((min(ix+ox,w-1),min(iy+oy,h-1)))*wx*wy for ox,wx in [(0,1-fx),(1,fx)] for oy,wy in [(0,1-fy),(1,fy)])/255*tint[3]/255
     old=result.get((px,py),(64,128,191));color=(0,0,0) if shadow else tuple(c*t/255 for c,t in zip(white,tint))
     result[px,py]=tuple(math.floor(src*a+dst*(1-a)+.5) for src,dst in zip(color,old))
  for c in text:
   draw(glyphs[c],x,y,True);draw(glyphs[c],x,y,False);x+=glyphs[c][3]*scale
  visible=[p for p in result if p not in prior and max(abs(a-b) for a,b in zip(result[p],(64,128,191)))>15]
  assert len(visible)>=8, (index,width,height,'label oracle has no meaningful visible pixels')
 changed=[(p,c) for p,c in result.items() if max(abs(a-b) for a,b in zip(c,(64,128,191)))>15]
 positions=[(f'label{i}',*p) for i,(p,c) in enumerate(changed[::max(1,len(changed)//100)])]
 positions.append(('background',5,5));result[5,5]=(64,128,191)
 return result,positions,descriptions
