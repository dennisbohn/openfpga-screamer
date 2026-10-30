#!/bin/sh
# Package the core and copy it onto the Pocket's SD card (Pocket in USB mode or card reader),
# together with the player's own original game files the core reads (core/data.json, slots
# 3-20), unchanged and in the game's own folders (DATA, GFX), into Assets/screamer/common/.
# Usage: tools/deploy.sh <SD card root> [game folder], e.g. tools/deploy.sh /i  (Git Bash for
#        drive I:); the game folder defaults to $GAME_DIR or "C:/GOG Games/Screamer"
# Only Screamer's own folders and files are written; everything else on the card stays as it is.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
card=${1:?usage: tools/deploy.sh <SD card root> [game folder]}
game=${2:-${GAME_DIR:-C:/GOG Games/Screamer}}
[ -d "$card/Cores" ] && [ -d "$card/Platforms" ] || { echo "$card does not look like a Pocket SD card"; exit 1; }
files="GFX/GAME.DAT GFX/MENU.DAT GFX/GRAF.DAT DATA/GEN.DAT DATA/LOCAL.DAT DATA/SOUND.DAT"
for n in 1 2 3 4 5 6; do files="$files GFX/MAP${n}H.DAT DATA/MAP$n.DAT"; done
for f in $files; do
    [ -f "$game/$f" ] || { echo "$game/$f is missing: name the game's folder (argument or GAME_DIR)"; exit 1; }
done

sh "$root/tools/package.sh" > /dev/null
src="$root/build/sdcard"
cp -r "$src/." "$card/"
for f in $files; do
    mkdir -p "$card/Assets/screamer/common/${f%/*}"
    cp "$game/$f" "$card/Assets/screamer/common/$f"
    rm -f "$card/Assets/screamer/common/${f#*/}"            # deployed flat for a while on 2026-10-28
done
rm -f "$card/Assets/screamer/Chainy.Screamer/track1.bin"     # made on the PC until 2026-10, now built by the core
sync

cmp -s "$src/Cores/Chainy.Screamer/bitstream.rbf_r" "$card/Cores/Chainy.Screamer/bitstream.rbf_r"
echo "Deployed to $card:"
(cd "$src" && find . -type f | sed 's|^\./|  |' | sort)
for f in $files; do
    echo "  Assets/screamer/common/$f  (from $game)"
done
