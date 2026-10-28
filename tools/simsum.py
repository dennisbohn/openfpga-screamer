"""A SoC simulation log (tools/sim.sh soc ...) in a few lines, against a baseline.

Usage: python tools/simsum.py <log> [--save]
  prints the verdict, the pixel check, loading and building the track, and the first race
  frame's counts, each with its change against build/sim/baseline.json; --save makes this run
  the baseline.
"""
import json
import os
import re
import sys

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "sim", "baseline.json")
COUNTS = ("pieces", "faces", "px", "cpu", "geometry", "setup", "raster", "misses", "blit", "frame")


def counts(line):
    out = {}
    for k in COUNTS:
        if k == "frame":
            m = re.search(r"fps \d+, frame (\d+)", line)
            if m:
                out[k] = int(m.group(1))
            continue
        m = re.search(r"(\d+) " + k + r"\b" if k in ("pieces", "faces", "px") else r"\b" + k + r" (\d+)", line)
        if m:
            out[k] = int(m.group(1))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    log, save = sys.argv[1], "--save" in sys.argv
    text = open(log, encoding="latin-1").read()
    lines = text.splitlines()
    verdict = next((l for l in lines if l.startswith(("PASS", "FAIL"))), "no verdict (did it time out or crash?)")
    print(verdict[:110])
    for l in lines:
        if l.startswith(("%Error", "%Warning")) or "error:" in l:
            print(" ", l[:160])
    first = lambda p: next((l for l in lines if l.startswith(p)), None)
    for p in ("track check:", "game files:", "track image:"):
        l = first(p)
        if l:
            print(" ", re.sub(r" (MAP1H.DAT|GAME.DAT|MAP1.DAT|GEN.DAT|LOCAL.DAT) \d+", "", l)[:150])
    race = first("track frame 2:")
    now = counts(race) if race else {}
    base = {}
    if os.path.exists(BASE):
        base = json.load(open(BASE))
    if now:
        parts = []
        for k in COUNTS:
            if k in now:
                d = now[k] - base[k] if k in base else None
                parts.append(f"{k} {now[k]}" + (f" ({d:+d})" if d else ""))
        print("  frame 2:", ", ".join(parts))
    if save and now:
        os.makedirs(os.path.dirname(BASE), exist_ok=True)
        json.dump(now, open(BASE, "w"))
        print("  (saved as the baseline)")


main()
