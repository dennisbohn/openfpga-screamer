"""Prints the Pocket's debug log (debug.txt, data slot 1) in a few lines.

Usage: python -I tools/pocketlog.py [debug.txt]
       default file: I:/Saves/screamer/Chainy.Screamer/debug.txt (the SD card as drive I:)

Output: the header line, the lines about loading and the checks, any failure, and a table of the
race samples (one every 250 frames). The debug firmware (tools/fw.sh DEBUG=1) writes the log.
"""
import re
import sys

FRAME = 1485000         # clocks of a 50 Hz frame at 74.25 MHz


def field(line, pattern):
    """The first group of the regular expression in the line, or "?" if it does not match."""
    m = re.search(pattern, line)
    return m.group(1) if m else "?"


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    path = sys.argv[1] if len(sys.argv) > 1 else "I:/Saves/screamer/Chainy.Screamer/debug.txt"
    try:
        text = open(path, "rb").read().replace(b"\0", b"").decode("latin-1")
    except OSError as e:
        sys.exit(f"{path}: {e} (is the SD card in the PC?)")
    lines = [l.rstrip() for l in text.splitlines() if l.strip()]
    if lines:
        print(lines[0].strip())
    keys = ("game files:", "track image:", "track check:", "track:", "memtest: errors", "stack used")
    for l in lines[1:]:
        if l.startswith(keys) or re.search(r"FAIL|BAD|not loaded|error|trap", l, re.I):
            print(" ", l[:160])
    rows = [l for l in lines if l.startswith("track frame")]
    if rows:
        print("  frame   km/h  gear  place  faces  cars  fps  cycles      of a frame")
    for l in rows:
        cyc = field(l, r"fps \d+, frame (\d+)")
        pct = f"{int(cyc) * 100 // FRAME} %" if cyc != "?" else "?"
        print(f"  {field(l, r'track frame (\d+)'):>5}  {field(l, r'(\d+) km/h'):>5}"
              f"  {field(l, r'gear (\d+)'):>4}"
              f"  {field(l, r'place (\d+)'):>5}  {field(l, r'(\d+) faces'):>5}"
              f"  {field(l, r'cars in view (\d+)'):>4}"
              f"  {field(l, r'fps (\d+)'):>3}  {cyc:>9}  {pct:>6}")
    if len(text) >= 4000:
        print("  (the log is full: 4 KB, later samples are cut off)")


main()
