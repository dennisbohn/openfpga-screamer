#!/bin/sh
# Host test of the track build on the Pocket (firmware/track_build.c and its parts): the
# image built natively in Docker from the five original containers must be byte-identical
# with the one tools/make_track.py builds from the same files (the reference).
# Usage: tools/check_track_build.sh [game folder] [reference image]
#   game folder   the original game, default $GAME_DIR or "C:/GOG Games/Screamer"
#   reference     default: made here by tools/make_track.py into build/check/track1.bin
# Everything it writes stays in build/check/ (game data, never committed).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
game=${1:-${GAME_DIR:-C:/GOG Games/Screamer}}
image="verilator/verilator:v5.050"
out="$root/build/check"
mkdir -p "$out"
[ -f "$game/GFX/MAP1H.DAT" ] || { echo "$game does not look like the game's folder (no GFX/MAP1H.DAT)"; exit 1; }

if [ -n "$2" ]; then
    cp "$2" "$out/track1.bin"
else
    py=python3
    "$py" -c "" 2>/dev/null || py=python     # Windows: python3 may be the Store's stub
    "$py" "$root/tools/make_track.py" "$game/GFX/MAP1H.DAT" "$game/DATA/MAP1.DAT" "$game/DATA/GEN.DAT" \
        "$out/track1.bin" "$out/track1.png" > /dev/null
fi

# Git Bash on Windows: hand Docker Windows paths and stop MSYS rewriting /work.
if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$root")
    game_mount=$(cygpath -w "$game")
else
    mount=$root
    game_mount=$game
fi
src="firmware/track_build.c firmware/dat.c firmware/tb_mmp.c firmware/tb_spl.c firmware/tb_gfx.c \
     firmware/tb_hud.c firmware/crc32.c tools/check_track_build.c"
MSYS_NO_PATHCONV=1 docker run --rm -e ASAN_OPTIONS=detect_leaks=0 -v "$mount:/work" -v "$game_mount:/game:ro" -w /work --entrypoint sh "$image" -c "
    gcc -O2 -g -Wall -Wextra -Wdouble-promotion -fsanitize=address,undefined -fno-sanitize-recover \
        $src -lm -o build/check/check_track_build &&
    build/check/check_track_build /game build/check/track1.bin
"
