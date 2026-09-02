#!/usr/bin/env python3
"""Convert any image to a 400x240 1-bit P4 PBM for the SSTV transmitter.

usage: img2pbm.py input.(jpg|png|...) output.pbm
Cover-fits and centre-crops, stretches contrast, Floyd-Steinberg dithers.
"""
import sys
from PIL import Image, ImageOps

W, H = 400, 240


def convert(src: str, dst: str) -> None:
    img = Image.open(src).convert("L")
    img = ImageOps.fit(img, (W, H), method=Image.LANCZOS, centering=(0.5, 0.5))
    img = ImageOps.autocontrast(img, cutoff=1)
    bw = img.convert("1")  # Floyd-Steinberg
    bw.save(dst, format="PPM")  # mode "1" is written as P4


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    convert(sys.argv[1], sys.argv[2])
    print(f"wrote {sys.argv[2]} ({W}x{H} P4)")
