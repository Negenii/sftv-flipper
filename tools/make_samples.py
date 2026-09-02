#!/usr/bin/env python3
"""Generate sample .pbm images into images/. Run: tools/.venv/bin/python tools/make_samples.py"""
import math
import os
from PIL import Image, ImageDraw

W, H = 400, 240
OUT = os.path.join(os.path.dirname(__file__), "..", "images")


def save(img: Image.Image, name: str) -> None:
    os.makedirs(OUT, exist_ok=True)
    img.convert("1").save(os.path.join(OUT, name), format="PPM")
    print("wrote", name)


def test_pattern() -> Image.Image:
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    for i in range(8):  # gray bars, dithered later
        d.rectangle([i * 50, 0, i * 50 + 49, 60], fill=int(255 * i / 7))
    for x in range(0, W, 20):  # vertical lines of increasing thickness
        d.line([x, 70, x, 120], fill=0, width=1 + x // 100)
    d.ellipse([20, 130, 120, 230], outline=0, width=3)
    d.rectangle([150, 130, 250, 230], outline=0, width=3)
    d.polygon([(330, 130), (380, 230), (280, 230)], outline=0)
    d.text((10, 10), "SSTV 400x240", fill=255)
    return img


def gradient_sphere() -> Image.Image:
    img = Image.new("L", (W, H), 255)
    px = img.load()
    cx, cy, r = 200, 120, 100
    for y in range(H):
        for x in range(W):
            dx, dy = x - cx, y - cy
            dist = math.hypot(dx, dy)
            if dist < r:
                z = math.sqrt(r * r - dx * dx - dy * dy)
                shade = (dx * -0.5 + dy * -0.5 + z * 0.7) / r
                px[x, y] = int(max(0, min(255, 40 + 200 * shade)))
            else:
                px[x, y] = 255 if ((x // 40) + (y // 40)) % 2 else 210
    return img


def checker() -> Image.Image:
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    for y in range(0, H, 20):
        for x in range(0, W, 20):
            if ((x // 20) + (y // 20)) % 2 == 0:
                d.rectangle([x, y, x + 19, y + 19], fill=0)
    return img


if __name__ == "__main__":
    save(test_pattern(), "test_pattern.pbm")
    save(gradient_sphere(), "sphere.pbm")
    save(checker(), "checker.pbm")
