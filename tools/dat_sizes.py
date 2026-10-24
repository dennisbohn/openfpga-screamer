"""List Screamer .DAT container entries with stored and uncompressed sizes.

Usage: python dat_sizes.py [-v] <file.DAT>...
"""
import sys
from collections import defaultdict

import datfile


def main():
    verbose = sys.argv[1] == "-v"
    for path in sys.argv[2 if verbose else 1:]:
        ents = datfile.entries(datfile.read(path))
        total = sum(e.size for e in ents)
        by_ext = defaultdict(int)
        for e in ents:
            by_ext[e.name.rsplit(".", 1)[-1].upper()] += e.size
        exts = " ".join(f"{k}={v // 1024}K" for k, v in sorted(by_ext.items(), key=lambda kv: -kv[1]))
        print(f"{path.split('/')[-1]:12s} {len(ents):3d} entries  {total / 1024:8.0f} KB unpacked   {exts}")
        if verbose:
            for e in sorted(ents, key=lambda e: -e.size):
                print(f"    {e.name:16s} {e.size:9d} (stored {e.stored:9d}, m{e.method})")


main()
