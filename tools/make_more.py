#!/usr/bin/env python3
"""More royalty-free 400x240 1-bit patterns into images/ (blank, solid, and a dozen tilings).
Run: tools/.venv/bin/python tools/make_more.py
Then push to a Flipper: .../scripts/storage.py -p <port> send images/<name>.pbm /ext/apps_data/sftv_tx/<name>.pbm
"""
import math, os
from PIL import Image, ImageDraw

W, H = 400, 240
OUT = os.path.join(os.path.dirname(__file__), "..", "images")


def save(img, name):
    os.makedirs(OUT, exist_ok=True)
    img.convert("1").save(os.path.join(OUT, name), format="PPM")
    print("wrote", name)


def blank():
    save(Image.new("L", (W, H), 255), "white.pbm")


def solid():
    save(Image.new("L", (W, H), 0), "black.pbm")


def stripes_h():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for y in range(0, H, 16): d.rectangle([0, y, W, y + 7], fill=0)
    save(img, "stripes_h.pbm")


def stripes_v():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for x in range(0, W, 16): d.rectangle([x, 0, x + 7, H], fill=0)
    save(img, "stripes_v.pbm")


def diagonals():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for i in range(-H, W, 18): d.line([i, 0, i + H, H], fill=0, width=6)
    save(img, "diagonals.pbm")


def dots():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for y in range(12, H, 24):
        for x in range(12, W, 24): d.ellipse([x - 6, y - 6, x + 6, y + 6], fill=0)
    save(img, "dots.pbm")


def brick():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    bh, bw = 24, 60
    for r, y in enumerate(range(0, H, bh)):
        off = (bw // 2) if r % 2 else 0
        d.line([0, y, W, y], fill=0, width=2)
        for x in range(-off, W, bw): d.line([x, y, x, y + bh], fill=0, width=2)
    save(img, "brick.pbm")


def waves():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for base in range(-40, H + 40, 22):
        pts = [(x, base + int(14 * math.sin(x / 26.0))) for x in range(0, W + 1, 4)]
        d.line(pts, fill=0, width=3)
    save(img, "waves.pbm")


def spiral():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    cx, cy = W // 2, H // 2
    pts = []
    a = 0.0
    while a < 22 * math.pi:
        r = 2.2 * a
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        a += 0.15
    d.line(pts, fill=0, width=3)
    save(img, "spiral.pbm")


def concentric_sq():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    cx, cy = W // 2, H // 2
    for r in range(8, 130, 14):
        d.rectangle([cx - 5 * r // 3, cy - r, cx + 5 * r // 3, cy + r], outline=0, width=3)
    save(img, "squares.pbm")


def herringbone():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    s = 20
    for gy, y in enumerate(range(0, H, s)):
        for gx, x in enumerate(range(0, W, s)):
            if (gx + gy) % 2: d.line([x, y, x + s, y + s], fill=0, width=3)
            else: d.line([x, y + s, x + s, y], fill=0, width=3)
    save(img, "herringbone.pbm")


def crosshatch():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for i in range(-H, W, 14): d.line([i, 0, i + H, H], fill=0, width=2)
    for i in range(0, W + H, 14): d.line([i, 0, i - H, H], fill=0, width=2)
    save(img, "crosshatch.pbm")


def bullseye():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    cx, cy = W // 2, H // 2
    for i, r in enumerate(range(0, 260, 18)):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=0 if i % 2 else 255, outline=0)
    save(img, "bullseye.pbm")


def triangles():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    s = 40
    for gy, y in enumerate(range(0, H, s)):
        for x in range(0, W, s):
            up = [(x, y + s), (x + s // 2, y), (x + s, y + s)]
            if (x // s + gy) % 2: d.polygon(up, fill=0)
            else: d.polygon(up, outline=0)
    save(img, "triangles.pbm")


if __name__ == "__main__":
    for f in (blank, solid, stripes_h, stripes_v, diagonals, dots, brick, waves,
              spiral, concentric_sq, herringbone, crosshatch, bullseye, triangles):
        f()
