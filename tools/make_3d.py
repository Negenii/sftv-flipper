#!/usr/bin/env python3
"""Gradient and 3D-shaded 400x240 patterns into images/ (dithered to 1-bit on save).
Run: tools/.venv/bin/python tools/make_3d.py
Push: .../scripts/storage.py -p <port> send images/<name>.pbm /ext/apps_data/sftv_tx/<name>.pbm
"""
import math, os
from PIL import Image, ImageDraw

W, H = 400, 240
OUT = os.path.join(os.path.dirname(__file__), "..", "images")


def save(img, name):
    os.makedirs(OUT, exist_ok=True)
    img.convert("1").save(os.path.join(OUT, name), format="PPM")  # Floyd-Steinberg dither
    print("wrote", name)


def clampf(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def gradient_radial():
    img = Image.new("L", (W, H)); px = img.load()
    cx, cy, mx = W / 2, H / 2, math.hypot(W / 2, H / 2)
    for y in range(H):
        for x in range(W):
            px[x, y] = clampf(255 * (1 - math.hypot(x - cx, y - cy) / mx))
    save(img, "grad_radial.pbm")


def gradient_diag():
    img = Image.new("L", (W, H)); px = img.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = clampf(255 * (x + y) / (W + H))
    save(img, "grad_diag.pbm")


def sphere3d():
    img = Image.new("L", (W, H), 235); px = img.load()
    cx, cy, r = W / 2, H / 2, 100
    lx, ly, lz = -0.5, -0.6, 0.62
    for y in range(H):
        for x in range(W):
            dx, dy = (x - cx) / r, (y - cy) / r
            if dx * dx + dy * dy <= 1:
                dz = math.sqrt(max(0.0, 1 - dx * dx - dy * dy))
                diff = max(0, dx * lx + dy * ly + dz * lz)
                spec = max(0, diff) ** 12
                px[x, y] = clampf(40 + 200 * diff + 120 * spec)
    save(img, "sphere3d.pbm")


def torus3d():
    img = Image.new("L", (W, H), 240); px = img.load()
    cx, cy = W / 2, H / 2
    R, rr = 90, 38
    lx, ly, lz = -0.4, -0.5, 0.77
    for y in range(H):
        for x in range(W):
            dx, dy = x - cx, (y - cy) * 1.7  # tilt
            d = math.hypot(dx, dy)
            if abs(d - R) <= rr:
                a = (d - R) / rr
                nz = math.sqrt(max(0, 1 - a * a))
                ux, uy = dx / (d + 1e-6), dy / (d + 1e-6)
                nx, ny = ux * a, uy * a
                diff = max(0, nx * lx + ny * ly + nz * lz)
                px[x, y] = clampf(35 + 210 * diff)
    save(img, "torus3d.pbm")


def cube_iso():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    cx, cy, s = W / 2, H / 2, 80
    top = [(cx, cy - s), (cx + s, cy - s / 2), (cx, cy), (cx - s, cy - s / 2)]
    left = [(cx - s, cy - s / 2), (cx, cy), (cx, cy + s), (cx - s, cy + s / 2)]
    right = [(cx + s, cy - s / 2), (cx, cy), (cx, cy + s), (cx + s, cy + s / 2)]
    d.polygon(top, fill=210, outline=0)
    d.polygon(left, fill=90, outline=0)
    d.polygon(right, fill=150, outline=0)
    save(img, "cube_iso.pbm")


def cylinder3d():
    img = Image.new("L", (W, H), 245); px = img.load()
    cx, r = W / 2, 90
    top, bot = 30, H - 30
    for y in range(top, bot):
        for x in range(W):
            dx = (x - cx) / r
            if -1 <= dx <= 1:
                nz = math.sqrt(1 - dx * dx)
                diff = max(0, -0.5 * dx + 0.86 * nz)
                px[x, y] = clampf(30 + 220 * diff)
    d = ImageDraw.Draw(img)
    d.ellipse([cx - r, top - 14, cx + r, top + 14], fill=200, outline=0)
    save(img, "cylinder3d.pbm")


def cone3d():
    img = Image.new("L", (W, H), 250); px = img.load()
    cx, cy, base_r, top_y = W / 2, H - 30, 90, 30
    height = cy - top_y
    for y in range(top_y, int(cy)):
        t = (y - top_y) / height
        rr = base_r * t
        for x in range(W):
            dx = x - cx
            if abs(dx) <= rr and rr > 0:
                nx = dx / rr
                diff = max(0, -0.55 * nx + 0.83 * math.sqrt(max(0, 1 - nx * nx)))
                px[x, y] = clampf(30 + 215 * diff)
    save(img, "cone3d.pbm")


def floor_persp():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    horizon = 70
    vx = W / 2
    for i in range(-20, 21):  # vertical lines converging
        d.line([vx + i * 20, H, vx + i * 3, horizon], fill=0, width=1)
    for k in range(1, 24):  # horizontal lines, spacing shrinks toward horizon
        y = horizon + (H - horizon) * (1 - 1 / (1 + k * 0.35))
        d.line([0, y, W, y], fill=0, width=1)
    d.rectangle([0, 0, W, horizon], fill=30)  # dark sky
    save(img, "floor_persp.pbm")


def tunnel():
    img = Image.new("L", (W, H)); px = img.load()
    cx, cy = W / 2, H / 2
    for y in range(H):
        for x in range(W):
            d = math.hypot(x - cx, y - cy)
            px[x, y] = clampf(128 + 127 * math.sin(d / 7.0))
    save(img, "tunnel.pbm")


def wave3d():
    img = Image.new("L", (W, H), 255); d = ImageDraw.Draw(img)
    for gx in range(0, W + 1, 12):
        pts = [(gx, 40 + y * 0.7 + 22 * math.sin((gx + y) / 30.0)) for y in range(0, H)]
        d.line(pts, fill=0)
    for gy in range(40, H, 14):
        pts = [(x, gy + 22 * math.sin((x + gy) / 30.0)) for x in range(0, W + 1, 6)]
        d.line(pts, fill=0)
    save(img, "wave3d.pbm")


def metaballs():
    img = Image.new("L", (W, H)); px = img.load()
    balls = [(140, 120, 70), (260, 110, 60), (200, 170, 45)]
    for y in range(H):
        for x in range(W):
            v = 0.0
            for bx, by, br in balls:
                v += br * br / ((x - bx) ** 2 + (y - by) ** 2 + 1)
            px[x, y] = clampf(255 * min(1.0, v))
    save(img, "metaballs.pbm")


if __name__ == "__main__":
    for f in (gradient_radial, gradient_diag, sphere3d, torus3d, cube_iso,
              cylinder3d, cone3d, floor_persp, tunnel, wave3d, metaballs):
        f()
