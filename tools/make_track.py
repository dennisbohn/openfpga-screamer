"""One track for the Pocket's track flythrough (firmware/track.c), from the game files.

Usage: python tools/make_track.py <GFX/MAPnL.DAT> <DATA/MAPn.DAT> <out.bin> <preview.png>

Formats from openscreamer docs/NOTES.md ("How the track is built", sections 1-4 and 11).
The output, for data slot 3 (core/data.json), little-endian:

  0x00  "SCRK", u32 version 1
  0x08  u32 offset, size of MAPn.MLB     materials and pixel pages, unchanged
  0x10  u32 offset, size of MAPn.UNR     the meshes, unchanged
  0x18  u32 offset of the palette        256 x 0x00RRGGBB (MAPn.COL is 6-bit VGA)
  0x1C  u32 offset, count of placements  from MAPn.MMP: int32 object, X, Y, height, heading
                                         (2048 = one turn), 20 bytes each
  0x24  u32 offset, count of camera path points: int32 X, Y, height, about 100 units apart,
                                         along the player's line (MAPnOPP.SPL, the tenth
                                         spline, a uniform quadratic B-spline over the
                                         nodes' handles)

Sections start on 256-byte boundaries. The output is made from game data: it stays in
build/, never in the repository. The preview draws placements and path from above.
"""
import math
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


def placements(mmp):
    """The 64-byte records of runs 0 and 1 of every cell of the 256 x 256 grid"""
    i32 = lambda o: struct.unpack_from("<i", mmp, o)[0]
    out = []
    for gy in range(256):
        row = i32(4 * gy)
        if row == -1:
            continue
        for gx in range(256):
            c = i32(row + 4 * gx)
            if c == -1:
                continue
            n = i32(c + 0x14) + i32(c + 0x18)
            for k in range(n):
                o = c + 0x28 + 64 * k
                out.append((i32(o), i32(o + 0x08), i32(o + 0x10), i32(o + 0x0C), i32(o + 0x18)))
    return out


def splines(spl):
    """[(start_node, [node dicts])] of a .SPL file holding several splines"""
    out, p = [], 0
    while p + 8 <= len(spl):
        count, start = struct.unpack_from("<2i", spl, p)
        p += 8
        nodes = []
        for k in range(count):
            f = struct.unpack_from("<14i", spl, p + 56 * k)
            nodes.append({"x": f[1], "y": f[2], "z": f[3], "next": f[4],
                          "in": (f[5], f[6]), "out": (f[8], f[9])})
        p += 56 * count
        out.append((start, nodes))
    return out


def player_path(spl, step=100.0):
    start, nodes = splines(spl)[9]
    pts, k = [], start
    for _ in range(len(nodes)):
        n, m = nodes[k], nodes[nodes[k]["next"]]
        for piece in range(2):
            a, b, c = ((n["in"], n["out"], m["in"]) if piece == 0 else (n["out"], m["in"], m["out"]))
            h0, h1 = (n["z"], n["z"]) if piece == 0 else (n["z"], m["z"])
            length = math.dist(a, b) + math.dist(b, c)
            steps = max(1, int(length / 2 / step))
            for s in range(steps):
                u = s / steps
                wa, wc = (1 - u) ** 2 / 2, u * u / 2
                wb = 1 - wa - wc
                pts.append((round(a[0] * wa + b[0] * wb + c[0] * wc),
                            round(a[1] * wa + b[1] * wb + c[1] * wc),
                            round(h0 + (h1 - h0) * u)))
        k = n["next"]
    return pts


def main():
    gfx, dat, out, preview = sys.argv[1:5]
    g = datfile.read(gfx)
    track = os.path.basename(gfx).upper().split("L.DAT")[0]          # MAP1
    mlb = datfile.extract(g, track + ".MLB")
    unr = datfile.extract(g, track + ".UNR")
    col = datfile.extract(g, track + ".COL")
    mmp = datfile.extract(g, track + ".MMP")
    spl = datfile.extract(datfile.read(dat), track + "OPP.SPL")
    pal = b"".join(struct.pack("<I", col[3 * i] * 4 << 16 | col[3 * i + 1] * 4 << 8 | col[3 * i + 2] * 4)
                   for i in range(256))
    pl = placements(mmp)
    path = player_path(spl)

    sections = [mlb, unr, pal, b"".join(struct.pack("<5i", *p) for p in pl),
                b"".join(struct.pack("<3i", *p) for p in path)]
    offs, pos = [], 0x100
    for s in sections:
        offs.append(pos)
        pos = (pos + len(s) + 255) & ~255
    head = b"SCRK" + struct.pack("<I", 1) + struct.pack("<2I", offs[0], len(mlb)) + \
        struct.pack("<2I", offs[1], len(unr)) + struct.pack("<I", offs[2]) + \
        struct.pack("<2I", offs[3], len(pl)) + struct.pack("<2I", offs[4], len(path))
    blob = bytearray(pos)
    blob[:len(head)] = head
    for o, s in zip(offs, sections):
        blob[o:o + len(s)] = s
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(blob)

    # preview from above: placements grey, the path red
    xs = [p[1] for p in pl] + [p[0] for p in path]
    ys = [p[2] for p in pl] + [p[1] for p in path]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    W = 800
    H = int(W * (y1 - y0) / (x1 - x0)) + 1
    img = bytearray(b"\x10\x10\x10" * W * H)

    def dot(x, y, c, r=1):
        px = int((x - x0) / (x1 - x0) * (W - 1))
        py = int((y1 - y) / (y1 - y0) * (H - 1))
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if 0 <= px + dx < W and 0 <= py + dy < H:
                    o = ((py + dy) * W + px + dx) * 3
                    img[o:o + 3] = c

    for p in pl:
        dot(p[1], p[2], b"\xa0\xa0\xa0", 2)
    for p in path:
        dot(p[0], p[1], b"\xff\x30\x30", 0)
    png(preview, W, H, bytes(img))
    print(f"{out}: {len(blob)} bytes: MLB {len(mlb)}, UNR {len(unr)}, {len(pl)} placements, "
          f"{len(path)} path points; preview {preview}")


main()
