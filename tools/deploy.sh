#!/bin/sh
# Package the core and copy it onto the Pocket's SD card (Pocket in USB mode or card reader).
# Usage: tools/deploy.sh <SD card root>, e.g. tools/deploy.sh /i  (Git Bash for drive I:)
# Only Screamer's own folders and files are written; everything else on the card stays as it is.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
card=${1:?usage: tools/deploy.sh <SD card root>}
[ -d "$card/Cores" ] && [ -d "$card/Platforms" ] || { echo "$card does not look like a Pocket SD card"; exit 1; }

sh "$root/tools/package.sh" > /dev/null
src="$root/build/sdcard"
cp -r "$src/." "$card/"
sync

cmp -s "$src/Cores/Chainy.Screamer/bitstream.rbf_r" "$card/Cores/Chainy.Screamer/bitstream.rbf_r"
echo "Deployed to $card:"
(cd "$src" && find . -type f | sed 's|^\./|  |' | sort)
