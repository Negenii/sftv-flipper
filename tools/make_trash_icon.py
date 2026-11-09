"""Trash-can frames for the "Deleted" notice in SFTV TX: lid opens, a sheet drops in, lid shuts.
Usage: python3 tools/make_trash_icon.py flipper/images   (writes trash_0.png .. trash_10.png, 23x24)"""
import math, sys
from PIL import Image, ImageDraw
W, H, O = 23, 24, 4  # O: everything moved down by O rows
OUT = sys.argv[1]
TOP = 8 + O  # can rim row
def can(d):
    d.line([(3, TOP), (16, TOP)], fill=0)
    d.line([(3, TOP), (4, 19 + O)], fill=0)
    d.line([(16, TOP), (15, 19 + O)], fill=0)
    d.line([(4, 19 + O), (15, 19 + O)], fill=0)
    for x in (7, 10, 12):
        d.line([(x, 10 + O), (x, 17 + O)], fill=0)
def lid(img, ang, lift=0):
    pts = [(x, 6 + O) for x in range(2, 18)] + [(x, 7 + O) for x in range(2, 18)] + \
          [(8, 5 + O), (8, 4 + O), (9, 4 + O), (10, 4 + O), (11, 4 + O), (11, 5 + O)]
    a = math.radians(ang); hx, hy = 17.9, 7.9 + O
    for (x, y) in pts:
        for sx in (0.25, 0.75):
            for sy in (0.25, 0.75):
                px, py = x + sx - hx, y + sy - hy
                rx = hx + px * math.cos(a) - py * math.sin(a)
                ry = hy + px * math.sin(a) + py * math.cos(a) - lift
                ix, iy = int(math.floor(rx)), int(math.floor(ry))
                if 0 <= ix < W and 0 <= iy < H: img.putpixel((ix, iy), 0)
def sheet(img, y):
    s = Image.new('1', (W, H), 1); d = ImageDraw.Draw(s)
    x = 6
    d.rectangle([x, y, x + 6, y + 7], fill=1, outline=0)
    d.line([(x + 4, y), (x + 6, y + 2)], fill=0)
    d.point((x + 5, y), fill=1); d.point((x + 6, y), fill=1); d.point((x + 6, y + 1), fill=1)
    d.point((x + 4, y + 1), fill=0); d.point((x + 4, y + 2), fill=0); d.point((x + 5, y + 2), fill=0)
    for yy in range(0, TOP):  # only above the rim: it drops behind the front wall
        for xx in range(W):
            if s.getpixel((xx, yy)) == 0: img.putpixel((xx, yy), 0)
# (lid angle, lift, sheet top)
seq = [(0, 0, None), (40, 0, None), (80, 0, None), (80, 0, 0), (80, 0, 3), (80, 0, 6), (80, 0, 9),
       (40, 0, None), (0, 0, None), (0, 1, None), (0, 0, None)]
for i, (ang, lift, sy) in enumerate(seq):
    im = Image.new('1', (W, H), 1); d = ImageDraw.Draw(im)
    can(d)
    if sy is not None: sheet(im, sy)
    lid(im, ang, lift)
    im.save(f'{OUT}/trash_{i}.png')
