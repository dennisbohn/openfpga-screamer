"""The release archive from build/sdcard (tools/package.sh): build/release/Chainy.Screamer_<version>.zip.

Usage: python -I tools/release_zip.py

The version is the one in core/core.json. The archive holds the folders for the root of the
Pocket's SD card (Cores/, Platforms/, Assets/) with forward-slash names and fixed time stamps,
so the same files give the same archive. It never holds game data: the script stops if a file
has the extension of one of the game's own.
"""
import hashlib
import json
import os
import sys
import zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "build", "sdcard")
GAME = (".dat", ".gog", ".pcx", ".pkc", ".spr", ".mlb", ".unr", ".3dx", ".raw", ".hmp", ".tab", ".bnk")

version = json.load(open(os.path.join(root, "core", "core.json"), encoding="utf-8"))["core"]["metadata"]["version"]
files = sorted(os.path.relpath(os.path.join(d, f), src).replace(os.sep, "/")
               for d, _, names in os.walk(src) for f in names)
if not files:
    sys.exit("build/sdcard is empty: run tools/package.sh first")
bad = [f for f in files if f.lower().endswith(GAME)]
if bad:
    sys.exit("game data in build/sdcard: " + ", ".join(bad))

out_dir = os.path.join(root, "build", "release")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "Chainy.Screamer_%s.zip" % version)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for f in files:
        info = zipfile.ZipInfo(f, date_time=(2026, 11, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(os.path.join(src, f), "rb") as fh:
            z.writestr(info, fh.read())
data = open(out, "rb").read()
print("%s: %d files, %d bytes, sha256 %s" % (out, len(files), len(data), hashlib.sha256(data).hexdigest()))
for f in files:
    print("  " + f)
