#!/bin/sh
# Packages the core (tools/package.sh) and copies it onto the Pocket's SD card, together with the
# player's own game files that the core reads.
# Usage: tools/deploy.sh <SD card root> [game folder]
#   tools/deploy.sh /i      Git Bash, the Pocket in USB mode or a card reader as drive I:
#   The game folder defaults to $GAME_DIR, then to "C:/GOG Games/Screamer".
# Game files: the 21 files that core/data.json names (data slots 3 to 22 and 25), unchanged. They keep
# the game's own folders (DATA, GFX) and go to Assets/screamer/common/. GOG's image of the CD,
# SCREAM.gog, and its cue sheet SCREAM.DAT lie at the top of that folder. A file that is already on
# the card with the same size is left as it is (the CD image has 383 MB).
# Only Screamer's own folders and files are written; everything else on the card stays as it is.
# Needs: the SD card with Cores/ and Platforms/ (a Pocket card), the built firmware and bitstream.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
card=${1:?usage: tools/deploy.sh <SD card root> [game folder]}
game=${2:-${GAME_DIR:-C:/GOG Games/Screamer}}
[ -d "$card/Cores" ] && [ -d "$card/Platforms" ] || { echo "$card does not look like a Pocket SD card"; exit 1; }
files="GFX/GAME.DAT GFX/MENU.DAT GFX/GRAF.DAT DATA/GEN.DAT DATA/LOCAL.DAT DATA/SOUND.DAT DATA/MUSIC.DAT SCREAM.DAT SCREAM.gog"
for n in 1 2 3 4 5 6; do files="$files GFX/MAP${n}H.DAT DATA/MAP$n.DAT"; done
for f in $files; do
    [ -f "$game/$f" ] || { echo "$game/$f is missing: name the game's folder (argument or GAME_DIR)"; exit 1; }
done

# Assemble build/sdcard/ and copy all of it onto the card.
sh "$root/tools/package.sh" > /dev/null
src="$root/build/sdcard"
cp -r "$src/." "$card/"
for f in $files; do
    dst="$card/Assets/screamer/common/$f"
    mkdir -p "$(dirname "$dst")"
    [ -f "$dst" ] && [ "$(stat -c %s "$game/$f")" = "$(stat -c %s "$dst")" ] || cp "$game/$f" "$dst"
    # A file of DATA/ or GFX/ belongs in its folder only: remove a copy directly in common/.
    case "$f" in */*) rm -f "$card/Assets/screamer/common/${f#*/}" ;; esac   # deployed flat for a while on 2026-10-28
done
# The core builds the track image itself, so a track1.bin on the card is not used: remove it.
rm -f "$card/Assets/screamer/Chainy.Screamer/track1.bin"     # made on the PC until 2026-10, now built by the core
sync

# With set -e the script stops here, without a message, if the card holds another bitstream.
cmp -s "$src/Cores/Chainy.Screamer/bitstream.rbf_r" "$card/Cores/Chainy.Screamer/bitstream.rbf_r"
echo "Deployed to $card:"
(cd "$src" && find . -type f | sed 's|^\./|  |' | sort)
for f in $files; do
    echo "  Assets/screamer/common/$f  (from $game)"
done
