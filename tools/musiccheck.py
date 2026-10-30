"""Check the music in a simulation's sound (tools/sim.sh SIM_WAV) against the CD track itself.

Usage: python -I tools/musiccheck.py <wav> <SCREAM.gog> <SCREAM.DAT> <CD track> [--exact]

The mixer (core/src/fpga/core/audio.v, firmware/mixer.c) plays the track from its first frame:
output frame j is the track at j x 60211 / 65536 frames, the two frames around it in a straight
line (fraction 16 bits, rounded down), times 0x4000 >> 15, clipped to 16 bits.
--exact, for a run without the effects (SOUND.DAT left out of SIM_GAME): from the first frame
with sound on, every frame of the file must be exactly that. Without it the effects lie on top:
the music is found by correlation (left + right) and fitted, gain and what is left.
Both: frames clipped, the longest run of the same frame repeated, frame by frame.
"""
import math
import os
import re
import sys
import wave

STEP, VOL, RATE = 60211, 0x4000, 48000     # VOL: firmware/music.c MASTER


def track_bytes(cue, image_size, n):
    """The track's first and end byte in the image: INDEX 01 to the next track's, or the end."""
    first, cur = {}, 0
    for ln in open(cue, encoding="latin-1"):
        t = ln.split()
        if len(t) >= 2 and t[0] == "TRACK":
            cur = int(t[1])
        elif len(t) >= 3 and t[0] == "INDEX" and int(t[1]) == 1:
            mm, ss, ff = (int(x) for x in t[2].split(":"))
            first[cur] = (mm * 60 + ss) * 75 + ff
    later = [s for k, s in first.items() if k > n]
    return first[n] * 2352, (min(later) * 2352 if later else image_size)


def expected(pcm, frames):
    """[(left, right)] the mixer makes of the track's frames pcm[(l, r)], frames of them"""
    out = []
    for j in range(frames):
        pos = j * STEP
        i, f = pos >> 16, pos & 0xFFFF
        lr = []
        for k in range(2):
            s0, s1 = pcm[i][k], pcm[i + 1][k]
            v = (s0 + ((s1 - s0) * f >> 16)) * VOL >> 15
            lr.append(max(-32768, min(32767, v)))
        out.append(tuple(lr))
    return out


def main():
    wav, image, cue, n = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    exact = "--exact" in sys.argv
    w = wave.open(wav, "rb")
    raw = w.readframes(w.getnframes())
    got = [(int.from_bytes(raw[i:i + 2], "little", signed=True), int.from_bytes(raw[i + 2:i + 4], "little", signed=True))
           for i in range(0, len(raw), 4)]
    a, b = track_bytes(cue, os.path.getsize(image), n)
    need = len(got) * STEP // 65536 + 4
    with open(image, "rb") as f:
        f.seek(a)
        data = f.read(min(need * 4, b - a))
    pcm = [(int.from_bytes(data[i:i + 2], "little", signed=True), int.from_bytes(data[i + 2:i + 4], "little", signed=True))
           for i in range(0, len(data) - 3, 4)]
    m = expected(pcm, min(len(got), (len(pcm) - 2) * 65536 // STEP))
    m0 = next(j for j, v in enumerate(m) if v != (0, 0))
    print(f"{wav}: {len(got)} frames ({len(got) / RATE:.2f} s); CD track {n}: bytes {a} to {b}, "
          f"its first sound at output frame {m0}")

    if exact:
        w0 = next(j for j, v in enumerate(got) if v != (0, 0))
        off = w0 - m0
        cmp = [(j, got[off + j], m[j]) for j in range(len(m)) if 0 <= off + j < len(got)]
        bad = [c for c in cmp if c[1] != c[2]]
        print(f"music from file frame {off} ({off / RATE:.3f} s): {len(cmp)} frames ({len(cmp) / RATE:.2f} s) "
              f"compared, {len(bad)} differ")
        for j, g, e in bad[:5]:
            print(f"  frame {off + j} (track frame {j * STEP / 65536:.1f}): file {g}, expected {e}")
    else:
        # coarse (every 16th frame, 0.5 s), then fine around the best
        mono = [l + r for l, r in got]
        mm = [l + r for l, r in m]
        span = RATE // 2
        ref = mm[m0:m0 + span]

        def corr(k, d):
            s = sum(mono[k + i] * ref[i] for i in range(0, span, d))
            return s

        best = max(range(0, len(mono) - span, 16), key=lambda k: corr(k, 16))
        best = max(range(max(0, best - 16), min(len(mono) - span, best + 17)), key=lambda k: corr(k, 1))
        off = best - m0
        pairs = [(mono[off + j], mm[j]) for j in range(m0, len(m)) if off + j < len(mono)]
        gain = sum(g * e for g, e in pairs) / max(1, sum(e * e for _, e in pairs))
        rms = lambda xs: math.sqrt(sum(x * x for x in xs) / max(1, len(xs)))
        print(f"music from file frame {off} ({off / RATE:.3f} s), {len(pairs)} frames: gain {gain:.4f}, "
              f"rms file {rms([g for g, _ in pairs]):.0f}, music {rms([e for _, e in pairs]):.0f}, "
              f"file - music {rms([g - e for g, e in pairs]):.0f} (left + right)")

    clipped = sum(1 for l, r in got if l in (32767, -32768) or r in (32767, -32768))
    peak = max(max(abs(l), abs(r)) for l, r in got)
    run = longest = 1
    for j in range(1, len(got)):
        run = run + 1 if got[j] == got[j - 1] and got[j] != (0, 0) else 1
        longest = max(longest, run)
    print(f"peak {peak}, {clipped} frames clipped, longest run of one sounding frame repeated: {longest}")


if __name__ == "__main__":
    main()
