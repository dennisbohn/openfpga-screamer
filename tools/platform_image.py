"""The Pocket's platform image (Platforms/_images/<platform>.bin) from a PNG, and back.

Usage: python -I tools/platform_image.py <in.png> <out.bin>      PNG -> .bin
       python -I tools/platform_image.py --show <in.bin> <out.png>  .bin -> PNG (as the Pocket shows it)

The format (Analogue's "Graphical Asset Formats", checked against the images on the SD card):
521 x 165 pixels, 2 bytes a pixel, the brightness in the first byte and 0 in the second; the
picture turned 90 degrees counter-clockwise, so the file is 165 pixels wide and 521 high; and the
brightness stored inverted (the cores' images are negatives of what the Pocket shows: 255 is
black on the screen). The PNG may be grey, palette or RGB(A), 8 bits a sample; colour is turned
into grey (0.299 R + 0.587 G + 0.114 B). Draw it as it should look on the Pocket.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # (python -I leaves it out)
from cmpframe import read_png, write_png

W, H = 521, 165


def to_bin(png, out):
    w, h, px = read_png(png)
    if (w, h) != (W, H):
        sys.exit(f"{png}: {w} x {h}, the Pocket wants {W} x {H}")
    grey = [round(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) for p in px]
    b = bytearray(W * H * 2)
    for y in range(H):
        for x in range(W):
            i = ((W - 1 - x) * H + y) * 2           # turned counter-clockwise
            b[i] = 255 - grey[y * W + x]            # inverted
    open(out, "wb").write(b)
    print(f"{out}: {len(b)} bytes")


def to_png(binfile, out):
    d = open(binfile, "rb").read()
    if len(d) != W * H * 2:
        sys.exit(f"{binfile}: {len(d)} bytes, not {W * H * 2}")
    rows = bytearray()
    for y in range(H):
        for x in range(W):
            g = 255 - d[((W - 1 - x) * H + y) * 2]
            rows += bytes((g, g, g))
    write_png(out, W, H, bytes(rows))
    print(f"{out}: {W} x {H}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--show":
        to_png(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3:
        to_bin(sys.argv[1], sys.argv[2])
    else:
        sys.exit(__doc__)
