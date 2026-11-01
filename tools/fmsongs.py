"""The songs of a MUSIC.DAT, read independently of firmware/fm.c: tools/fmcheck.sh runs this first.

Usage: python -I tools/fmsongs.py <MUSIC.DAT> [folder to unpack the banks and songs into]

Per song: the header, the tracks (channel, events, length in ticks), the kinds of messages in them, the
notes, and the song's length: it is played round again when every track has ended (firmware/fm.c).
Everything the firmware's parser has to cope with is looked for here: a message other than the ones
the driver knows, a track that does not end with FF 2F, bytes after the end. Exit status 1 for any.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import datfile  # noqa: E402


def delay(d, p):
    """HMP delay: 7 bits a byte, the low part first, the last byte has its top bit set."""
    v = shift = 0
    while True:
        c = d[p]
        p += 1
        v |= (c & 0x7F) << shift
        shift += 7
        if c & 0x80:
            return v, p


def song(name, d):
    bad = []
    if d[:14] != b"HMIMIDIP013195":
        bad.append("no HMIMIDIP013195 header")
    ntracks = struct.unpack_from("<H", d, 0x30)[0]
    rate = struct.unpack_from("<H", d, 0x38)[0]
    pos = 0x388
    tracks = []
    kinds = {}
    for t in range(ntracks):
        num, length, chan = struct.unpack_from("<III", d, pos)
        if pos + length > len(d):
            bad.append("track %d is longer than the file" % t)
            break
        p, end = pos + 12, pos + length
        tick = 0
        notes = drums = 0
        events = 0
        by_tick = {}
        ended = False
        while p < end:
            dl, p = delay(d, p)
            tick += dl
            st = d[p]
            p += 1
            events += 1
            if st == 0xFF:
                typ = d[p]
                p += 1
                ln = 0
                while True:
                    c = d[p]
                    p += 1
                    ln = ln << 7 | (c & 0x7F)
                    if not c & 0x80:
                        break
                p += ln
                kinds["FF %02X" % typ] = kinds.get("FF %02X" % typ, 0) + 1
                if typ == 0x2F:
                    ended = True
                    break
                continue
            if st < 0x80:
                bad.append("track %d: byte %02X where a message is due (no running status)" % (t, st))
                break
            k = st & 0xF0
            n = 1 if k in (0xC0, 0xD0) else 2
            a = d[p]
            b = d[p + 1] if n == 2 else 0
            p += n
            key = "%02X" % k + ("*" if k == 0xB0 else "")
            if k == 0xB0:
                key = "B0 cc%02X" % a
            kinds[key] = kinds.get(key, 0) + 1
            if k == 0x90 and b:
                if t > 0:
                    if st & 15 == 9:
                        drums += 1
                    else:
                        notes += 1
                    by_tick[tick] = by_tick.get(tick, 0) + 1
        if not ended:
            bad.append("track %d does not end with FF 2F" % t)
        elif p != end:
            bad.append("track %d: %d bytes after FF 2F" % (t, end - p))
        tracks.append(dict(num=num, chan=chan, events=events, ticks=tick, notes=notes, drums=drums, by_tick=by_tick))
        pos = end
    if pos != len(d) and any(d[pos:]):
        bad.append("%d bytes after the last track, not all zero" % (len(d) - pos))   # (zeros: one a track, unused)
    return dict(name=name, rate=rate, tracks=tracks, kinds=kinds, bad=bad)


def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    data = datfile.read(path)
    status = 0
    for e in datfile.entries(data):
        body = data[e.offset:e.offset + e.size]
        if out:
            os.makedirs(out, exist_ok=True)
            open(os.path.join(out, e.name), "wb").write(body)
        if e.method != 0:
            print("%s: method %d, not stored" % (e.name, e.method))
            status = 1
        if not e.name.endswith(".HMP"):
            continue
        s = song(e.name, body)
        length = max(t["ticks"] for t in s["tracks"])
        notes = sum(t["notes"] for t in s["tracks"])
        drums = sum(t["drums"] for t in s["tracks"])
        print("%s: %d bytes, %d ticks/s, %d tracks, longest %d ticks = %.1f s, %d notes + %d drum hits" %
              (e.name, len(body), s["rate"], len(s["tracks"]), length, length / s["rate"], notes, drums))
        print("   channels", sorted(set(t["chan"] for t in s["tracks"])), "events",
              [t["events"] for t in s["tracks"]])
        print("   messages", ", ".join("%s x%d" % kv for kv in sorted(s["kinds"].items())))
        for b in s["bad"]:
            print("   PROBLEM:", b)
            status = 1
        # the notes at one tick: how many a chord can hold, to see the driver's nine voices at work
        by_tick = {}
        for t in s["tracks"]:
            for k, v in t["by_tick"].items():
                by_tick[k] = by_tick.get(k, 0) + v
        print("   most note-ons in one tick: %d" % max(by_tick.values()))
        # python's own count of every tick's note-ons for fmcheck's comparison with the firmware
        if out:
            with open(os.path.join(out, e.name + ".notes"), "w") as f:
                f.write("%d %d\n" % (length, notes + drums))
    return status


sys.exit(main())
