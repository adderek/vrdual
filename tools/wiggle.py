#!/usr/bin/env python3
"""Side-by-side PNG -> wiggle GIF (left/right eye alternating, ~7 Hz).

  wiggle.py SBS.png OUT.gif [WIDTH]     prints "dx" (alignment shift, px) on stdout

The right eye is shifted horizontally so the centre of the image lines up
(the dominant object stands still, the rest of the scene rocks), then both
eyes are cropped to the common overlap so the edges don't flicker. No
dithering: per-frame dither noise would flicker on its own. Pillow only.
"""
import sys
from PIL import Image, ImageChops, ImageStat

src, out = sys.argv[1], sys.argv[2]
width = int(sys.argv[3]) if len(sys.argv) > 3 else 960
sbs = Image.open(src).convert("RGB")
w, h = sbs.size
ew0 = w // 2
L, R = sbs.crop((0, 0, ew0, h)), sbs.crop((ew0, 0, 2 * ew0, h))

# search on a 1/4 grey copy, centre region only (HUD lives at the edges)
k = 4
a, b = (e.convert("L").resize((ew0 // k, h // k)) for e in (L, R))
W, H = a.size
box = (W // 5, H // 5, W * 4 // 5, H * 4 // 5)
ac = a.crop(box)

def cost(d):
    bc = b.crop((box[0] + d, box[1], box[2] + d, box[3]))
    return ImageStat.Stat(ImageChops.difference(ac, bc)).mean[0]

dx = min(range(-W // 8, W // 8 + 1), key=cost) * k
ew = ew0 - abs(dx)
lx, rx = (0, dx) if dx >= 0 else (-dx, 0)
eyes = (L.crop((lx, 0, lx + ew, h)), R.crop((rx, 0, rx + ew, h)))
size = (width, round(h * width / ew))
frames = [e.resize(size, Image.LANCZOS) for e in eyes]
pal = frames[0].quantize(colors=256, method=Image.Quantize.MEDIANCUT)
frames = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
frames[0].save(out, save_all=True, append_images=frames[1:], duration=140, loop=0, optimize=True)
print(dx)
