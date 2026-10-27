"""One track for the Pocket (firmware/track.c, car.c), from the game files.

Usage: python tools/make_track.py <GFX/MAPnL.DAT> <DATA/MAPn.DAT> <DATA/GEN.DAT> <out.bin>
                                  <preview.png>
(GFX/GAME.DAT and DATA/LOCAL.DAT are taken from beside them.)

Formats from openscreamer docs/NOTES.md ("How the track is built", sections 1-4, 6-9, 11).
The output, for data slot 3 (core/data.json), little-endian:

  0x00  "SCRK", u32 version 6
  0x08  u32 offset, size of MAPn.MLB     materials and pixel pages, unchanged
  0x10  u32 offset, size of MAPn.UNR     the meshes, unchanged
  0x18  u32 offset of the palette        256 x 0x00RRGGBB (MAPn.COL is 6-bit VGA)
  0x1C  u32 offset, count of placements  from MAPn.MMP: int32 object, X, Y, height, heading
                                         (2048 = one turn), 20 bytes each; cell by cell, each
                                         cell's record run 1 before its run 0, the order in which
                                         the game puts them into its draw list (0x83164)
  0x24  u32 offset, count of camera path points: int32 X, Y, height, about 100 units apart,
                                         along the player's line (MAPnOPP.SPL, the tenth
                                         spline, a uniform quadratic B-spline over the
                                         nodes' handles)
  0x2C  u32 offset, count of the ground index: the drivable pieces (record run 0) by the
                                         grid cell of their position, cell = ((Y + 2^20) >> 13)
                                         x 256 + ((X + 2^20) >> 13): u16 first[65537], then
                                         count u16 placement numbers, each cell's in file order
                                         (as openscreamer's track_under() walks them)
  0x34  u32 offset, count of markers     MAPn.MRK's 32-byte records unchanged
  0x3C  u32 offset, count of cars        CARS.DTR's CARS_DATA rows, 68 bytes each: char
                                         name[12], int32 livery, float maxspd, maxrpm, grip,
                                         accel, int32 automatic, nshift, float ratio[7]
  0x44  u32 offset, count of car models  AUTO00..: float wheelbase, front axle (ahead of the
                                         origin), rear axle (behind it), half track, from the
                                         wheel blocks' offsets; u32 offset (from this section)
                                         and size of the .3DX file, which follow the records
  0x4C  u32 offset, count of road points MAPn.MMP cell array 2: float angle, int32 x, y
  0x54  u32 offset, count of walls       MAPn.MMP cell array 3: float angle, int32 x, y, x1,
                                         y1, x2, y2
  0x5C  int32 x, y, float heading        where the player starts: its place on the grid, the
                                         start node of MAPnOPP.SPL's tenth spline (its own point),
                                         facing along that spline
  0x68  u32 offset, size of the race screen: u32 images, u32 fonts; per image u32 offset
                                         (from the section), u16 width, height, int16 x, y;
                                         per font u32 height, first character, count, widest,
                                         first image; the pictures' boxes (below); GAME.TXT's
                                         lines, 26 x 32 bytes; then the
                                         images, one byte a pixel, 0 see-through: the 63 sprites
                                         of GAMEL.SPR (x, y their offsets), the characters of
                                         GAMEL.FNT's fonts (each pixel its shade 1..15), then
                                         IMPLET_L and IMPRES_L (.PKC) each as the boxes round its
                                         groups of pixels (so the blitter does not walk the
                                         see-through between them), x, y the place on the
                                         screen; u32 first box, count of each of the two after
                                         the fonts
  0x70  int32 bonus_time, camera_hi      TRACK_DATA of MAPnB.DTR (the difficulty's file, below)
  0x78  u32 offset of the backdrop       MAPn.BCK (a PCX) unpacked: 512 x 256 palette
                                         indices, rows of 512 bytes, and its last row once more
                                         (the game's copy, 0x83a68, runs on past a row's end
                                         into the next row)
  0x7C  u32 ground colour                TRACK_DATA's color: the palette index below the
                                         backdrop
  0x80  u32 offset, size of the opponents: u32 splines, u32 rows; rows x float max_spd,
                                         accel, brakes, skill, sband (OPPONENTS of the
                                         difficulty's file); then per spline of MAPnOPP.SPL
                                         (opponent k drives spline k, the tenth is the player's)
                                         u32 start node, u32 nodes, nodes x int32 x, y, z (the
                                         node's own point), next, in x, y, out x, y (its handles)
  0x88  u32 file size, u32 n, n x u32    CRC-32 (zlib) of each 256 KB block from 0x100 on,
                                         so the core can tell where a load went wrong

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

BLOCK = 0x40000         # bytes per CRC block


def png(path, w, h, rgb):
    rows = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def cells(mmp):
    """The cells of the 256 x 256 grid that hold something, row by row"""
    i32 = lambda o: struct.unpack_from("<i", mmp, o)[0]
    for gy in range(256):
        row = i32(4 * gy)
        if row == -1:
            continue
        for gx in range(256):
            c = i32(row + 4 * gx)
            if c != -1:
                yield c


def placements(mmp):
    """The 64-byte records of runs 1 and 0 of every cell, and which of them are run 0"""
    i32 = lambda o: struct.unpack_from("<i", mmp, o)[0]
    out, run0 = [], []
    for c in cells(mmp):
        n0, n1 = i32(c + 0x14), i32(c + 0x18)
        for k in list(range(n0, n0 + n1)) + list(range(n0)):
            o = c + 0x28 + 64 * k
            out.append((i32(o), i32(o + 0x08), i32(o + 0x10), i32(o + 0x0C), i32(o + 0x18)))
            run0.append(k < n0)
    return out, run0


def ground_index(pl, run0):
    """u16 first[65537] and the run-0 placement numbers, binned by their position's cell"""
    bins = {}
    for i, (obj, x, y, h, t) in enumerate(pl):
        if not run0[i]:
            continue
        kx, ky = (x + 0x100000) >> 13, (y + 0x100000) >> 13
        if 0 <= kx < 256 and 0 <= ky < 256:
            bins.setdefault(ky * 256 + kx, []).append(i)
    first, order = [0] * 65537, []
    for k in range(65536):
        order += bins.get(k, [])
        first[k + 1] = len(order)
    assert len(order) < 65536 and len(pl) < 65536
    return struct.pack("<65537H", *first) + struct.pack(f"<{len(order)}H", *order), len(order)


def cell_arrays(mmp):
    """Arrays 2 (road points, 12 bytes) and 3 (walls, 28 bytes) of every cell"""
    i32 = lambda o: struct.unpack_from("<i", mmp, o)[0]
    dirs, walls = [], []
    for c in cells(mmp):
        for k in range(i32(c + 0x1C)):
            dirs.append(mmp[i32(c + 0x08) + 12 * k:i32(c + 0x08) + 12 * k + 12])
        for k in range(i32(c + 0x20)):
            walls.append(mmp[i32(c + 0x0C) + 28 * k:i32(c + 0x0C) + 28 * k + 28])
    return dirs, walls


def cars_data(dtr):
    """CARS.DTR's CARS_DATA rows (openscreamer drive.c, cars_dtr_load)"""
    rows, row, on = [], [], False
    for line in dtr.decode("latin-1").splitlines():
        s = line.strip()
        if not on:
            on = s.startswith("CARS_DATA")
            continue
        if s.startswith("END_LIST"):
            break
        for tok in s.split():
            if tok.startswith("*"):
                break
            row.append(tok)
            if len(row) == 14:
                rows.append(row)
                row = []
    num = lambda tok: int(tok[1:], 16) if tok[0] == "x" else float(tok[1:])
    out = b""
    for r in rows:
        ratio = [0.0] + [num(x) for x in r[8:14]]
        out += struct.pack("<12si4f2i7f", r[0][1:].encode()[:11], int(num(r[1])), num(r[2]), num(r[3]),
                           num(r[4]), num(r[5]), int(num(r[6])), int(num(r[7])), *ratio)
    return out, len(rows)


def car_model(x3d):
    """wheelbase, front axle, rear axle, half track of a .3DX car: from the offsets of its
    off-centre (wheel) blocks (openscreamer mesh.c); the nose is towards local -Z (16 bytes)"""
    i32 = lambda o: struct.unpack_from("<i", x3d, o)[0]
    lo = hi = half = 0.0
    seen = False
    for k in range(i32(0x14)):
        off = i32(0x18 + 4 * k)
        so = i32(off + 0x0C)
        if so <= 0:
            continue
        tx, tz = i32(so + 0x10), i32(so + 0x18)
        if tx == 0:
            continue
        lo, hi = (tz, tz) if not seen else (min(lo, tz), max(hi, tz))
        seen = True
        half = max(half, abs(tx))
    return struct.pack("<4f", hi - lo, -lo, hi, half) if seen else struct.pack("<4f", 935.0, 445.0, 490.0, 226.0)


def pkc(data):
    """A .PKC picture (320 x 200): negative = skip so many (see-through), positive = so many
    pixels follow; as the boxes round its groups of pixels: [(x, y, w, h, bytes)]"""
    px = bytearray(64000)
    p = at = 0
    while p + 4 <= len(data) and at < 64000:
        n = struct.unpack_from("<i", data, p)[0]
        p += 4
        if n < 0:
            at -= n
            continue
        px[at:at + n] = data[p:p + n]
        p += n
        at += n
    # groups: rows with pixels split into bands at empty rows, each band into boxes at runs
    # of more than 8 empty columns
    boxes = []
    rows = [y for y in range(200) if any(px[y * 320:(y + 1) * 320])]
    bands, start = [], None
    for y in range(201):
        on = y < 200 and y in rows
        if on and start is None:
            start = y
        if not on and start is not None:
            bands.append((start, y))
            start = None
    for y0, y1 in bands:
        cols = [x for x in range(320) if any(px[y * 320 + x] for y in range(y0, y1))]
        x0 = prev = cols[0]
        for x in cols[1:] + [10 ** 6]:
            if x - prev > 9:
                boxes.append((x0, y0, prev + 1 - x0, y1 - y0,
                              b"".join(bytes(px[y * 320 + x0:y * 320 + prev + 1]) for y in range(y0, y1))))
                x0 = x
            prev = x
    return boxes


def race_screen(game, local):
    """The race screen section (see the header): sprites, pictures, fonts, texts"""
    images, fonts = [], []
    spr = datfile.extract(game, "GAMEL.SPR")
    n, size = struct.unpack_from("<2i", spr, 0)
    pixels = 8 + 20 * n
    for k in range(n):
        ox, oy, w, h, at = struct.unpack_from("<5i", spr, 8 + 20 * k)
        images.append((ox, oy, w, h, spr[pixels + at:pixels + at + w * h]))
    fnt = datfile.extract(game, "GAMEL.FNT")
    nf, o = fnt[0] + 1, 1
    heads = []
    for i in range(nf):
        heads.append(struct.unpack_from("<3i", fnt, o))
        o += 12
    glyphs = []
    for h, first, count in heads:
        glyphs.append([struct.unpack_from("<2i", fnt, o + 8 * k) for k in range(count)])
        o += 8 * count
    o += 4                                              # the pixels' size
    for (h, first, count), gl in zip(heads, glyphs):
        fonts.append((h, first, count, max([1] + [w for w, _ in gl]), len(images)))
        for w, at in gl:                                # two pixels a byte, the high half first
            bw = (w + 1) // 2
            img = bytes((fnt[o + at + r * bw + c // 2] >> (0 if c & 1 else 4)) & 15
                        for r in range(h) for c in range(w))
            images.append((0, 0, w, h, img))
    pictures = []
    for name in ("IMPLET_L.PKC", "IMPRES_L.PKC"):
        boxes = pkc(datfile.extract(game, name))
        pictures.append((len(images), len(boxes)))
        images += boxes
    lines = datfile.extract(local, "GAME.TXT").decode("latin-1").splitlines()[:26]
    texts = b"".join(l.encode("latin-1")[:31].ljust(32, b"\0") for l in lines + [""] * (26 - len(lines)))
    head = 8 + 12 * len(images) + 20 * len(fonts) + 16 + len(texts)
    table, blob = b"", b""
    for x, y, w, h, img in images:
        table += struct.pack("<I2H2h", head + len(blob), w, h, x, y)
        blob += img
    return (struct.pack("<2I", len(images), len(fonts)) + table +
            b"".join(struct.pack("<5I", *f) for f in fonts) +
            b"".join(struct.pack("<2I", *pc) for pc in pictures) + texts + blob)


# the difficulty is the letter of the track's data file: 'A' + difficulty (0x13251), 0 ROOKIE,
# 1 NORMAL, 2 PRO; NORMAL is what the game's intro sets (0x2777f) and openscreamer's default
DIFFICULTY = "B"


def opponents(spl, dtr):
    """The opponents section: OPPONENTS rows of dtr, every spline of spl"""
    s = dtr.decode("latin-1")
    rows = []
    for line in s[s.index("OPPONENTS"):].splitlines()[1:]:
        f = line.split()
        if f and f[0].startswith("END_LIST"):
            break
        if len(f) >= 5 and all(v.startswith("f") for v in f[:5]):
            rows.append([float(v[1:]) for v in f[:5]])
    sp = splines(spl)
    out = struct.pack("<2I", len(sp), len(rows)) + b"".join(struct.pack("<5f", *r) for r in rows)
    for start, nodes in sp:
        out += struct.pack("<2I", start, len(nodes))
        for n in nodes:
            out += struct.pack("<8i", n["x"], n["y"], n["z"], n["next"], *n["in"], *n["out"])
    return out, len(sp), len(rows)


def grid_pose(spl):
    """The player's place on the grid: the start node of the tenth spline, its own point,
    facing along the curve there (piece 0 at u = 0 runs from the node's in handle to its out)"""
    start, nodes = splines(spl)[9]
    n = nodes[start]
    yaw = math.atan2(n["out"][0] - n["in"][0], -(n["out"][1] - n["in"][1]))
    return struct.pack("<2if", n["x"], n["y"], yaw)


def pcx(data):
    """A PCX picture (8 bits, one plane, run-length coded) as (width, height, indices)"""
    assert data[0] == 10 and data[2] == 1 and data[3] == 8 and data[65] == 1, "not an 8-bit PCX"
    x0, y0, x1, y1 = struct.unpack_from("<4H", data, 4)
    w, h, line = x1 - x0 + 1, y1 - y0 + 1, struct.unpack_from("<H", data, 66)[0]
    out, p = bytearray(), 128
    while len(out) < line * h:
        c = data[p]
        p += 1
        if c >= 0xC0:
            out += bytes([data[p]]) * (c & 0x3F)
            p += 1
        else:
            out.append(c)
    rows = [out[r * line:r * line + w] for r in range(h)]
    return w, h, b"".join(rows)


def track_data(dtr):
    """TRACK_DATA of MAPnA.DTR: color, camera_hi, bonus_time, carnage"""
    s = dtr.decode("latin-1")
    rows = [l.split() for l in s[s.index("TRACK_DATA"):].splitlines()[1:]]
    row = next(r for r in rows if r and not r[0].startswith("*"))
    return [int(v[1:]) for v in row[:4]]


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
    gfx, dat, gen, out, preview = sys.argv[1:6]
    g = datfile.read(gfx)
    d = datfile.read(dat)
    track = os.path.basename(gfx).upper()[:-5]                        # MAP1 from MAP1L.DAT or MAP1H.DAT
    mlb = datfile.extract(g, track + ".MLB")
    unr = datfile.extract(g, track + ".UNR")
    col = datfile.extract(g, track + ".COL")
    mmp = datfile.extract(g, track + ".MMP")
    spl = datfile.extract(d, track + "OPP.SPL")
    mrk = datfile.extract(d, track + ".MRK")
    pal = b"".join(struct.pack("<I", col[3 * i] * 4 << 16 | col[3 * i + 1] * 4 << 8 | col[3 * i + 2] * 4)
                   for i in range(256))
    pl, run0 = placements(mmp)
    path = player_path(spl)
    ground, nground = ground_index(pl, run0)
    nmark = struct.unpack_from("<i", mrk, 0)[0]
    cars, ncars = cars_data(datfile.extract(datfile.read(gen), "CARS.DTR"))
    x3d = [datfile.extract(g, "AUTO%02d.3DX" % k) for k in range(7)]
    models, files, at = b"", b"", 24 * len(x3d)
    for f in x3d:
        models += car_model(f) + struct.pack("<2I", at + len(files), len(f))
        files += f + b"\0" * (-len(f) % 256)
    models += files
    dirs, walls = cell_arrays(mmp)
    here = os.path.dirname(os.path.abspath(gfx))
    hud = race_screen(datfile.read(os.path.join(here, "GAME.DAT")),
                      datfile.read(os.path.join(here, "..", "DATA", "LOCAL.DAT")))
    dtr = datfile.extract(d, track + DIFFICULTY + ".DTR")
    colour, camera_hi, bonus_time, carnage = track_data(dtr)
    opp, nspl, nrows = opponents(spl, dtr)
    bw, bh, back = pcx(datfile.extract(g, track + ".BCK"))
    assert (bw, bh) == (512, 256), f"backdrop {bw} x {bh}"
    back += back[-512:]

    sections = [mlb, unr, pal, b"".join(struct.pack("<5i", *p) for p in pl),
                b"".join(struct.pack("<3i", *p) for p in path), ground, mrk[4:4 + 32 * nmark],
                cars, models, b"".join(dirs), b"".join(walls), hud, back, opp]
    counts = [len(mlb), len(unr), 0, len(pl), len(path), nground, nmark, ncars, 7, len(dirs), len(walls),
              len(hud)]
    offs, pos = [], 0x100
    for s in sections:
        offs.append(pos)
        pos = (pos + len(s) + 255) & ~255
    blob = bytearray(pos)
    for o, s in zip(offs, sections):
        blob[o:o + len(s)] = s
    crcs = [zlib.crc32(blob[o:o + BLOCK]) for o in range(0x100, pos, BLOCK)]
    head = b"SCRK" + struct.pack("<I", 6) + struct.pack("<2I", offs[0], counts[0]) + \
        struct.pack("<2I", offs[1], counts[1]) + struct.pack("<I", offs[2]) + \
        b"".join(struct.pack("<2I", offs[k], counts[k]) for k in range(3, 11)) + \
        grid_pose(spl) + struct.pack("<2I", offs[11], counts[11]) + \
        struct.pack("<2i", bonus_time, camera_hi) + struct.pack("<2I", offs[12], colour) + \
        struct.pack("<2I", offs[13], len(opp)) + \
        struct.pack("<2I", pos, len(crcs)) + struct.pack(f"<{len(crcs)}I", *crcs)
    assert len(head) == 0x90 + 4 * len(crcs)
    assert len(head) <= 0x100, "file too large for the block CRCs in the header"
    blob[:len(head)] = head
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
    print(f"{out}: {len(blob)} bytes: MLB {len(mlb)}, UNR {len(unr)}, {len(pl)} placements "
          f"({nground} drivable), {len(path)} path points, {nmark} markers, {ncars} cars, "
          f"{len(dirs)} road points, {len(walls)} walls, race screen {len(hud)} bytes "
          f"(bonus {bonus_time} s, camera {camera_hi}), backdrop {bw} x {bh} over colour {colour}, "
          f"{nspl} splines and {nrows} opponents (difficulty {DIFFICULTY}); "
          f"preview {preview}")


main()
