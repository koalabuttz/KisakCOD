"""Independent sparse UI composition oracle using exported metadata and Pillow.

No game data is embedded. Pillow decodes BC alpha/colors; RGB is adjusted for
legacy RGB565 32/64 expansion. The resulting rounding difference is bounded by
three channel values in comparisons, independently of observed screenshots.
"""
import io
from functools import lru_cache
import math
from pathlib import Path
import struct
from PIL import Image


def rgba_levels(path):
    data = Path(path).read_bytes()
    assert data[:4] == b'IWi\x06'
    fmt = data[4]
    fourcc = {11: b'DXT1', 12: b'DXT3', 13: b'DXT5'}.get(fmt)
    assert fmt == 3 or fourcc is not None
    width, height = struct.unpack_from('<HH', data, 6)
    end = len(data)
    levels = []
    while True:
        size = width*height*2 if fmt == 3 else max(1, (width+3)//4)*max(1, (height+3)//4)*(8 if fmt == 11 else 16)
        end -= size
        if fmt == 3:
            image = Image.frombytes('LA',(width,height),data[end:end+size]).convert('RGBA')
        else:
            header = [124, 0x81007, height, width, size, 0, 0]+[0]*11
            header += [32, 4, struct.unpack('<I', fourcc)[0], 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0]
            image = Image.open(io.BytesIO(b'DDS '+struct.pack('<31I', *header)+data[end:end+size])).convert('RGBA')
            # Independent decoder uses conventional 31/63 RGB565 denominators.
            image.putdata([tuple(math.floor(c*factor+.5) for c,factor in zip(pixel[:3],(31/32,63/64,31/32)))+(pixel[3],)
                           for pixel in image.getdata()])
        levels.append(image)
        if data[5] & 2 or (width == 1 and height == 1):
            break
        width, height = max(1,width//2), max(1,height//2)
    assert end == 28, (path, end)
    return levels


def sample(image, u, v):
    x = max(0,min(image.width-1,u*image.width-.5))
    y = max(0,min(image.height-1,v*image.height-.5))
    ix, iy = int(x), int(y)
    return tuple(sum(image.getpixel((min(ix+dx,image.width-1),min(iy+dy,image.height-1)))[channel]*wx*wy
                     for dx,wx in ((0,1-(x-ix)),(1,x-ix))
                     for dy,wy in ((0,1-(y-iy)),(1,y-iy))) for channel in range(4))


def blend(old, rgba, color):
    tint = [max(0,min(255,math.floor(c*255+.5))) for c in color]
    alpha = rgba[3]/255*tint[3]/255
    return tuple(math.floor(rgba[i]*tint[i]/255*alpha+old[i]*(1-alpha)+.5) for i in range(3))


class CompositionReference:
    def __init__(self, metadata, images, width, height):
        self.metadata, self.width, self.height = metadata, width, height
        self.materials = {entry['name']:entry for entry in metadata['materials']}
        self.images = {}
        for name, material in self.materials.items():
            image = material['textures'][0]['image']
            self.images[name] = ([Image.new('RGBA',(1,1),(255,255,255,255))] if image == '$white'
                                 else rgba_levels(Path(images)/(image+'.iwi')))
        self.sx, self.sy = min(width/640,height/480), height/480
        self.offset = (width-640*self.sx)/2

    def rectangle(self, rect):
        x,y,w,h = rect['xywh']
        hx,vy = rect['horz_align'],rect['vert_align']
        assert hx in (0,2,4) and vy in (0,1,3,4), (hx,vy)
        px = x*(self.width/640 if hx == 4 else self.sx)+(self.width/2 if hx == 2 else self.offset if hx == 0 else 0)
        py = y*self.sy+(self.height if vy == 3 else 0)
        return px,py,abs(w)*(self.width/640 if hx == 4 else self.sx),abs(h)*self.sy,w<0,h<0

    def draw_pixel(self, old, px, py, rectangle, material, color):
        x,y,w,h,flipx,flipy = rectangle
        if not (x <= px+.5 < x+w and y <= py+.5 < y+h):
            return old
        levels = self.images[material]
        mip = 0
        if self.materials[material]['textures'][0]['sampler_state'] == 235:
            ratio = max(levels[0].width/w,levels[0].height/h)
            mip = max(0,min(len(levels)-1,math.floor(math.log2(ratio)+.5)))
        u,v = (px+.5-x)/w,(py+.5-y)/h
        return blend(old,sample(levels[mip],1-u if flipx else u,1-v if flipy else v),color)

    @lru_cache(maxsize=65536)
    def background(self, px, py):
        result = (64,128,191)
        for item in self.metadata['items']:
            window = item['window']
            assert window['style'] == 3 and window['border'] == 0
            assert window['dynamic_flags'] & 4, 'oracle profile requires visible background windows'
            material = window['background']
            if material is None:
                expression = item['material_expression']
                assert len(expression) == 2 and expression[0]['op'] == 16, expression
                material = expression[1]['value']
            color = window['fore_color'] if window['dynamic_flags'] & 65536 else [1,1,1,1]
            result = self.draw_pixel(result,px,py,self.rectangle(window['rect']),material,color)
        return result

    def cursor(self, px, py, mouse_x, mouse_y, old=None):
        # Engine cursor dimensions use real placement; alignment4 maps its
        # center back to the exact absolute physical pointer coordinates.
        rectangle = (mouse_x-16*self.sx,mouse_y-16*self.sy,32*self.sx,32*self.sy,False,False)
        return self.draw_pixel(self.background(px,py) if old is None else old,px,py,rectangle,'ui_cursor',[1,1,1,1])
