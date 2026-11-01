#!/bin/sh
# Assemble the SD card layout in build/sdcard/ from core/:
#   Cores/Chainy.Screamer/     core JSONs, bitstream.rbf_r, icon.bin, info.txt
#   Platforms/screamer.json    platform definition
#   Platforms/_images/         platform image
#   Assets/screamer/Chainy.Screamer/  firmware.bin, program.bin (tools/fw.sh)
# Copy the contents of build/sdcard/ onto the root of the Pocket's SD card. A release never
# holds game data: the player copies the game's own files to Assets/screamer/common/
# (tools/deploy.sh does that from a game folder).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
core="$root/core"
out="$root/build/sdcard"
name="Chainy.Screamer"

rm -rf "$out"
mkdir -p "$out/Cores/$name" "$out/Platforms/_images"

for f in audio.json core.json data.json input.json interact.json variants.json video.json info.txt; do
    cp "$core/$f" "$out/Cores/$name/"
done
cp "$core/dist/icon.bin" "$out/Cores/$name/"
cp "$core/output/bitstream.rbf_r" "$out/Cores/$name/"
cp "$core/dist/platforms/screamer.json" "$out/Platforms/"
cp "$core/dist/platforms/_images/screamer.bin" "$out/Platforms/_images/"

# the firmware only: build/assets/ may also hold files made from the game data
assets="$root/build/assets/screamer/$name"
mkdir -p "$out/Assets/screamer/$name" "$out/Assets/screamer/common"      # common: empty, for the game files
for f in firmware.bin program.bin; do
    [ -f "$assets/$f" ] || { echo "$assets/$f is missing: run tools/fw.sh first"; exit 1; }
    cp "$assets/$f" "$out/Assets/screamer/$name/"
done

echo "SD card layout in $out:"
find "$out" -type f | sed "s|^$out/|  |" | sort
