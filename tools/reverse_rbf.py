"""Convert a Quartus .rbf into the bit-reversed .rbf_r the Analogue Pocket loads.

Usage: python reverse_rbf.py <in.rbf> <out.rbf_r>
"""
import sys

REVERSED = bytes(int(f"{b:08b}"[::-1], 2) for b in range(256))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as fh:
        data = fh.read()
    with open(dst, "wb") as fh:
        fh.write(data.translate(REVERSED))
    print(f"{dst}: {len(data)} bytes")


main()
