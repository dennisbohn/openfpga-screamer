"""Turn a raw 320x200 RGB frame from a testbench into a PNG.

Usage: python rgb2png.py <in.rgb> <out.png> [width height]
"""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (320, 200)
    raw = open(src, "rb").read()
    if len(raw) != w * h * 3:
        raise SystemExit(f"{src}: {len(raw)} bytes, expected {w * h * 3}")
    rows = b"".join(b"\0" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(dst, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    print(f"{dst}: {w} x {h}")


main()
