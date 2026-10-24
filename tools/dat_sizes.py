"""List Screamer .DAT container entries with stored and uncompressed sizes.

Format (from openscreamer docs/NOTES.md): [data][N x 32-byte records][u32 dir_size].
Record: 16-byte name XOR 0x3A, then u32 offset, stored_size, uncompressed_size, method.
"""
import struct
import sys
from collections import defaultdict


def entries(path):
    with open(path, "rb") as fh:
        data = fh.read()
    dir_size = struct.unpack_from("<I", data, len(data) - 4)[0]
    start = len(data) - 4 - dir_size
    out = []
    for pos in range(start, len(data) - 4, 32):
        raw = bytes(b ^ 0x3A for b in data[pos:pos + 16])
        name = raw.split(b"\0")[0].decode("latin-1")
        if not name:
            break
        off, stored, size, method = struct.unpack_from("<4I", data, pos + 16)
        out.append((name, stored, size, method))
    return out


def main():
    verbose = sys.argv[1] == "-v"
    for path in sys.argv[2 if verbose else 1:]:
        ents = entries(path)
        total = sum(e[2] for e in ents)
        by_ext = defaultdict(int)
        for name, _, size, _ in ents:
            by_ext[name.rsplit(".", 1)[-1].upper()] += size
        exts = " ".join(f"{k}={v // 1024}K" for k, v in sorted(by_ext.items(), key=lambda kv: -kv[1]))
        print(f"{path.split('/')[-1]:12s} {len(ents):3d} entries  {total / 1024:8.0f} KB unpacked   {exts}")
        if verbose:
            for name, stored, size, method in sorted(ents, key=lambda e: -e[2]):
                print(f"    {name:16s} {size:9d} (stored {stored:9d}, m{method})")


main()
