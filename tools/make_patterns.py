#!/usr/bin/env python3
"""Generate royalty-free 400x240 1-bit test patterns into images/.
Run: tools/.venv/bin/python tools/make_patterns.py
Then push to a Flipper: .../scripts/storage.py -p <port> send images/<name>.pbm /ext/apps_data/sftv_tx/<name>.pbm
"""
import math, os
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 240
OUT = os.path.join(os.path.dirname(__file__), "..", "images")


def font(sz):
    for p in ["/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Supplemental/Arial.ttf"]:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, sz)
            except Exception:
                pass
    return ImageFont.load_default()


def save(img, name):
    img.convert("1").save(os.path.join(OUT, name), format="PPM")
    print("wrote", name)


def rings():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    cx, cy = W // 2, H // 2
    for r in range(8, 260, 16):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=0, width=2)
    save(img, "rings.pbm")


def grid():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for x in range(0, W + 1, 20): d.line([x, 0, x, H], fill=0, width=1)
    for y in range(0, H + 1, 20): d.line([0, y, W, y], fill=0, width=1)
    d.line([W // 2, 0, W // 2, H], fill=0, width=3); d.line([0, H // 2, W, H // 2], fill=0, width=3)
    save(img, "grid.pbm")


def ramp():
    img = Image.new("L", (W, H), 0); d = ImageDraw.Draw(img)
    for i in range(16): d.rectangle([i * 25, 0, i * 25 + 24, H], fill=int(255 * i / 15))
    save(img, "ramp.pbm")


def sunburst():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    cx, cy = W // 2, H // 2
    for a in range(0, 360, 6):
        r = math.radians(a); d.line([cx, cy, cx + 400 * math.cos(r), cy + 400 * math.sin(r)], fill=0, width=2)
    d.ellipse([cx - 30, cy - 30, cx + 30, cy + 30], fill=255, outline=0, width=2)
    save(img, "sunburst.pbm")


def weave():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for x in range(-H, W, 16): d.line([x, 0, x + H, H], fill=0, width=2)
    for x in range(0, W + H, 16): d.line([x, 0, x - H, H], fill=0, width=1)
    save(img, "weave.pbm")


def halftone():
    img = Image.new("L", (W, H), 255); px = img.load()
    cx, cy, R = W // 2, H // 2, 150
    for y in range(H):
        for x in range(W):
            px[x, y] = max(0, min(255, int(255 * (math.hypot(x - cx, y - cy) / R))))
    save(img, "halftone.pbm")


def typography():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    d.text((14, 8), "SFTV", font=font(90), fill=0)
    d.text((16, 110), "Slow Flipper TV", font=font(30), fill=0)
    small = font(15)
    lines = ["Flipper Zero -> Playdate over sound.", "400x240, 1-bit, tones on the buzzer.",
             "The quick brown fox jumps over the", "lazy dog. 0123456789 !?@#%&*()"]
    for i, t in enumerate(lines): d.text((16, 150 + i * 20), t, font=small, fill=0)
    save(img, "type.pbm")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for f in (rings, grid, ramp, sunburst, weave, halftone, typography):
        f()
