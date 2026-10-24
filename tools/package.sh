#!/bin/sh
# Assemble the SD card layout in build/sdcard/ from core/:
#   Cores/Chainy.Screamer/     core JSONs, bitstream.rbf_r, icon.bin, info.txt
#   Platforms/screamer.json    platform definition
#   Platforms/_images/         platform image
#   Assets/screamer/...        files made from the game data, if build/assets/ has any
# Copy the contents of build/sdcard/ onto the root of the Pocket's SD card.
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

# Files made from the game data (tools/make_screen.py ...) wait in build/assets/,
# never in the repository.
if [ -d "$root/build/assets" ]; then
    mkdir -p "$out/Assets"
    cp -r "$root/build/assets/." "$out/Assets/"
fi

echo "SD card layout in $out:"
find "$out" -type f | sed "s|^$out/|  |" | sort
