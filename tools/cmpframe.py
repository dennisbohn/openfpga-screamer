#!/usr/bin/env python3
"""
cmpframe.py - one picture of the original against ours, pixel for pixel, as numbers.

The original's picture is a raw DOSBox screenshot (Ctrl+F5 with default_image_capture_formats
= raw, 320 x 200, the game's own pixels); ours a frame of the simulation (build/sim/<name>.rgb)
or a PNG. Two pixels count as the same when their colours agree in the VGA's 6 bits per channel
(DOSBox and our video path widen those 6 bits differently). Prints the share of equal pixels in
all, in the 3D view without the race screen's corners, and as a map of 40 x 40 tiles (a digit
per tile: tenths equal, '#' all of it); writes a picture of the differences (the original in
grey, differing pixels magenta) when asked. A screenshot with a palette is taken by its colour
numbers through the track's own palette (MAPn.COL), as the game dims its palette while paused.

Usage: python -I tools/cmpframe.py <original.png> <ours.rgb|png> [diff.png] [track]
    track: 1-6 (default 1), for the palette, from $GAME_DIR (default C:/GOG Games/Screamer)
"""
import os, struct, sys, zlib

sys.stdout.reconfigure(encoding="utf-8")
W, H = 320, 200


def read_png(path, index=False):
    d = open(path, "rb").read()
    i, idat, pal = 8, b"", None
    while i < len(d):
        n, t = struct.unpack(">I4s", d[i:i + 8])
        body = d[i + 8:i + 8 + n]
        i += 12 + n
        if t == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", body[:10])
            assert bd == 8, "8 bits per sample only"
        elif t == b"PLTE":
            pal = [tuple(body[k:k + 3]) for k in range(0, len(body), 3)]
        elif t == b"IDAT":
            idat += body
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    raw, st = zlib.decompress(idat), w * bpp
    px, prev = [], bytearray(st)
    for y in range(h):
        f, line = raw[y * (st + 1)], bytearray(raw[y * (st + 1) + 1:(y + 1) * (st + 1)])
        for x in range(st):
            a = line[x - bpp] if x >= bpp else 0
            b, c = prev[x], prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            if ct == 3: px.append(pal[line[x]] if not index else line[x])
            elif ct in (0, 4): px.append((line[x * bpp],) * 3)
            else: px.append(tuple(line[x * bpp:x * bpp + 3]))
        prev = line
    return w, h, px


def read_any(path):
    if path.lower().endswith(".rgb"):
        d = open(path, "rb").read()
        assert len(d) == W * H * 3, "%s: not 320 x 200 RGB" % path
        return W, H, [tuple(d[i:i + 3]) for i in range(0, len(d), 3)]
    return read_png(path)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda k, b: struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    a, b = read_png(sys.argv[1], index=True), read_any(sys.argv[2])
    assert a[:2] == (W, H) and b[:2] == (W, H), "both must be 320 x 200"
    if isinstance(a[2][0], int):        # colour numbers: the track's palette, 6 bits
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import datfile
        track = sys.argv[4] if len(sys.argv) > 4 else "1"
        game = os.environ.get("GAME_DIR", "C:/GOG Games/Screamer")
        col = datfile.extract(datfile.read(game + "/GFX/MAP%sH.DAT" % track), "MAP%s.COL" % track)
        a = a[:2] + ([tuple(c << 2 for c in col[3 * i:3 * i + 3]) for i in a[2]],)
    same = [all(p >> 2 == q >> 2 for p, q in zip(pa, pb)) for pa, pb in zip(a[2], b[2])]
    # the race screen's corners: lap and times, position, the rev counter, the radar
    hud = lambda x, y: (x < 90 and y < 110) or (x >= 250 and y < 45) or (x >= 200 and y >= 125) \
        or (x < 80 and y >= 120) or (110 <= x < 210 and y < 40)
    view = [same[y * W + x] for y in range(H) for x in range(W) if not hud(x, y)]
    print("equal pixels: %.1f %% of all, %.1f %% of the 3D view without the race screen"
          % (100 * sum(same) / len(same), 100 * sum(view) / len(view)))
    print("40 x 40 tiles, tenths equal (# all):")
    for ty in range(0, H, 40):
        row = ""
        for tx in range(0, W, 40):
            t = [same[y * W + x] for y in range(ty, ty + 40) for x in range(tx, tx + 40)]
            r = sum(t) / len(t)
            row += "#" if r == 1 else str(min(9, int(r * 10)))
        print("  " + row)
    if len(sys.argv) > 3:
        out = bytearray()
        for (r, g, bl), s in zip(a[2], same):
            v = (r + g + bl) // 6
            out += bytes((v, v, v)) if s else b"\xff\x00\xff"
        write_png(sys.argv[3], W, H, bytes(out))


if __name__ == "__main__":
    main()
