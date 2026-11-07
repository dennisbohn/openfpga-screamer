"""Converts a Quartus .rbf into the bit-reversed .rbf_r that the Analogue Pocket loads.

Usage: python -I tools/reverse_rbf.py <in.rbf> <out.rbf_r>
Every byte of the file is mirrored (bit 0 becomes bit 7). tools/build.sh calls this script.
"""
import sys

REVERSED = bytes(int(f"{b:08b}"[::-1], 2) for b in range(256))


def main():
    """Mirrors the bits of every byte of the input file and writes the output file."""
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as fh:
        data = fh.read()
    with open(dst, "wb") as fh:
        fh.write(data.translate(REVERSED))
    print(f"{dst}: {len(data)} bytes")


main()
