"""Zips the SD card layout of build/sdcard into build/release/Chainy.Screamer_<version>.zip.

Usage: python -I tools/release_zip.py      (run tools/package.sh first)

Reads: build/sdcard/ (tools/package.sh) and the version in core/core.json.
Writes: build/release/Chainy.Screamer_<version>.zip, and prints its size, its SHA-256 and the file list.

The archive holds the folders for the root of the Pocket's SD card (Cores/, Platforms/, Assets/),
with forward-slash names, empty folders included, and fixed time stamps, so the same files give
the same archive. It never holds game data: the script stops if a file has one of the extensions
in GAME, the extensions of the game's own files.
"""
import hashlib
import json
import os
import sys
import zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "build", "sdcard")
# Extensions of the game's own files; none of them may go into the archive.
GAME = (".dat", ".gog", ".pcx", ".pkc", ".spr", ".mlb", ".unr", ".3dx", ".raw", ".hmp", ".tab", ".bnk")

version = json.load(open(os.path.join(root, "core", "core.json"),
                         encoding="utf-8"))["core"]["metadata"]["version"]
files = sorted(os.path.relpath(os.path.join(d, f), src).replace(os.sep, "/")
               for d, _, names in os.walk(src) for f in names)
# Empty folders are listed too, as directory entries.
dirs = sorted(os.path.relpath(os.path.join(d, n), src).replace(os.sep, "/") + "/"
              for d, subs, names in os.walk(src) for n in subs if not os.listdir(os.path.join(d, n)))
if not files:
    sys.exit("build/sdcard is empty: run tools/package.sh first")
bad = [f for f in files if f.lower().endswith(GAME)]
if bad:
    sys.exit("game data in build/sdcard: " + ", ".join(bad))

out_dir = os.path.join(root, "build", "release")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "Chainy.Screamer_%s.zip" % version)
# Every entry gets the same date, so that the archive does not depend on when the files were made.
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dn in dirs:
        info = zipfile.ZipInfo(dn, date_time=(2026, 11, 1, 0, 0, 0))
        info.external_attr = (0o40755 << 16) | 0x10
        z.writestr(info, b"")
    for f in files:
        info = zipfile.ZipInfo(f, date_time=(2026, 11, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(os.path.join(src, f), "rb") as fh:
            z.writestr(info, fh.read())
data = open(out, "rb").read()
print("%s: %d files, %d bytes, sha256 %s" % (out, len(files), len(data), hashlib.sha256(data).hexdigest()))
for f in dirs + files:
    print("  " + f)
