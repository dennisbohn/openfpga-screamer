"""Textures of one track for the Pocket's texture demo (firmware/main.c), from the game files.

Usage: python tools/make_textures.py <MAPnL.DAT> <out.bin> <preview.png> <material>...
       python tools/make_textures.py <MAPnL.DAT> --sheet <preview.png> [first last]

Reads MAPn.MLB and MAPn.COL from the container (openscreamer docs/NOTES.md: material record
i at i * 16 = [routine, texture record offset, U extent, V extent]; texture record +8 = data
offset of frame 0; pixels in rows of 256 bytes) and writes, for data slot 3 (core/data.json):

  0x000  "SCRT", u32 count (at most 16)
  0x008  count x { u32 routine (16 solid, 64 colour 0 see-through), u32 width, u32 height,
         u32 offset of the page }
  0x400  palette: 256 x u32 0x00RRGGBB (MAPn.COL is 6-bit VGA)
  0x800  the pages, 256 x height bytes each, one after the other

The output is made from game data: it stays in build/, never in the repository.
--sheet draws materials first..last with their numbers into one picture to choose from.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import datfile


def png(path, w, h, rgb):
    rows = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def load(dat_path):
    data = datfile.read(dat_path)
    track = os.path.basename(dat_path).upper().split("L.DAT")[0]     # MAP1
    mlb = datfile.extract(data, track + ".MLB")
    col = datfile.extract(data, track + ".COL")
    pal = [(col[3 * i] * 4, col[3 * i + 1] * 4, col[3 * i + 2] * 4) for i in range(256)]
    i32 = lambda o: struct.unpack_from("<i", mlb, o)[0]
    mats, end, i = {}, len(mlb), 0
    while (i + 1) * 16 <= end:
        kind, rec, w, h = (i32(i * 16 + 4 * k) for k in range(4))
        if 0 < rec < len(mlb) - 12:
            end = min(end, rec)
            mats[i] = (kind, w, h, i32(rec + 8))
        i += 1
    return mlb, pal, mats


def page(mlb, mat):
    kind, w, h, off = mat
    h = min(h, 256)
    return mlb[off:off + 256 * h].ljust(256 * h, b"\0"), h


def main():
    args = sys.argv[1:]
    mlb, pal, mats = load(args[0])
    if args[1] == "--sheet":
        first, last = (int(args[3]), int(args[4])) if len(args) > 4 else (0, 59)
        chosen = [m for m in sorted(mats) if first <= m <= last]
        cols, cw, ch = 6, 264, 264
        W, H = cols * cw, ((len(chosen) + cols - 1) // cols) * ch
        img = bytearray(b"\x20\x20\x20" * W * H)
        for n, m in enumerate(chosen):
            px, py = (n % cols) * cw + 4, (n // cols) * ch + 4
            pg, h = page(mlb, mats[m])
            w = min(mats[m][1], 256)
            for y in range(h):
                for x in range(w):
                    o = ((py + y) * W + px + x) * 3
                    img[o:o + 3] = bytes(pal[pg[y * 256 + x]])
            for d in range(m + 1):              # material number as a row of dots above it
                o = ((py - 3) * W + px + 3 * d) * 3
                if px + 3 * d < (n % cols + 1) * cw:
                    img[o:o + 3] = b"\xff\xff\x00"
        png(args[2], W, H, bytes(img))
        for m in chosen:
            print(f"mat {m:3d}: routine {mats[m][0]:3d}, {mats[m][1]} x {mats[m][2]}")
        print(f"{args[2]}: {W} x {H}")
        return

    out, preview, numbers = args[1], args[2], [int(a) for a in args[3:]]
    if not 1 <= len(numbers) <= 16:
        raise SystemExit("1 to 16 materials")
    head = b"SCRT" + struct.pack("<I", len(numbers))
    body, offset = b"", 0x800
    for m in numbers:
        kind, w, h, _ = mats[m]
        pg, h = page(mlb, mats[m])
        head += struct.pack("<4I", kind, min(w, 256), h, offset)
        body += pg
        offset += len(pg)
    blob = head.ljust(0x400, b"\0") + b"".join(struct.pack("<I", r << 16 | g << 8 | b) for r, g, b in pal) + body
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(blob)
    # preview: the pages side by side at their full 256-byte width
    H = max(page(mlb, mats[m])[1] for m in numbers)
    W = 256 * len(numbers)
    img = bytearray(W * H * 3)
    for n, m in enumerate(numbers):
        pg, h = page(mlb, mats[m])
        for y in range(h):
            for x in range(256):
                o = (y * W + n * 256 + x) * 3
                img[o:o + 3] = bytes(pal[pg[y * 256 + x]])
    png(preview, W, H, bytes(img))
    print(f"{out}: {len(blob)} bytes, materials {numbers}; preview {preview}")


main()
