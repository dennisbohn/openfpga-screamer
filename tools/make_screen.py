"""Turns one of Screamer's 320x200 PCX pictures into a screen.bin file, the input of the display testbench.

Usage: python tools/make_screen.py <GRAF.DAT> <NAME.PCX> <screen.bin> [preview.png]
  (without -I: the script imports datfile.py from its own folder)
  GRAF.DAT    the container that holds the picture
  NAME.PCX    the picture in it, 320 x 200 pixels, 8 bits, run-length coded
  screen.bin  the output; preview.png, if given, is the picture as a PNG

screen.bin has 65,024 bytes. The display testbench (sim/display, docs/development.md) writes two
such files through its bridge window to the two frame buffers, starting at bridge address 0:
  0x0000-0x03FF  palette, 256 x 4 bytes: R, G, B, 0 (8 bits each)
  0x0400-...     pixels, 320 x 200 palette indices, row by row
"""
import os
import struct
import sys
import zlib

import datfile

WIDTH, HEIGHT = 320, 200


def decode_pcx(pcx):
    """Decodes a 320 x 200 PCX picture; returns the pixels (palette indices) and the 768-byte palette."""
    xmin, ymin, xmax, ymax = struct.unpack_from("<4H", pcx, 4)
    bpp, planes, stride = pcx[3], pcx[65], struct.unpack_from("<H", pcx, 66)[0]
    w, h = xmax - xmin + 1, ymax - ymin + 1
    if (pcx[0], pcx[2], bpp, planes) != (0x0A, 1, 8, 1) or (w, h) != (WIDTH, HEIGHT):
        raise ValueError(f"not a 320x200 8-bit RLE PCX ({w}x{h}, {bpp} bpp, {planes} planes)")
    if pcx[-769] != 0x0C:
        raise ValueError("no 256-colour palette at the end")

    rows, pos = [], 128
    for _ in range(h):
        row = bytearray()
        while len(row) < stride:
            b = pcx[pos]
            pos += 1
            if b >= 0xC0:
                row += bytes([pcx[pos]]) * (b & 0x3F)
                pos += 1
            else:
                row.append(b)
        rows.append(bytes(row[:w]))
    return b"".join(rows), pcx[-768:]


def write_png(path, pixels, palette):
    """Writes the picture (palette indices and the RGB palette) as a PNG file."""
    rgb = b"".join(b"\0" + b"".join(palette[i * 3:i * 3 + 3] for i in pixels[y * WIDTH:(y + 1) * WIDTH])
                   for y in range(HEIGHT))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(rgb)) + chunk(b"IEND", b""))


def main():
    """Extracts the picture from the container and writes screen.bin and, if asked, the preview."""
    graf, name, out = sys.argv[1:4]
    pixels, palette = decode_pcx(datfile.extract(datfile.read(graf), name))
    for path in sys.argv[3:5]:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(out, "wb") as fh:
        for i in range(256):
            fh.write(palette[i * 3:i * 3 + 3] + b"\0")
        fh.write(pixels)
    print(f"{out}: {name}, {1024 + len(pixels)} bytes")
    if len(sys.argv) > 4:
        write_png(sys.argv[4], pixels, palette)


main()
