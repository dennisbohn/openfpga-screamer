#!/bin/sh
# The FM music on the PC: firmware/fm.c (tools/host/fmdump.c) plays every song of the game's
# MUSIC.DAT against a model of the hardware's write queue, and its register writes are compared,
# tick by tick, with the writes of openscreamer's driver (port/src/music.c, GPL-3.0, from the
# game's HMIMDRV.386), which is built here from where it is (copied into build/ only).
# tools/fmsongs.py reads the songs independently first (tracks, messages, lengths, notes).
# Usage: tools/fmcheck.sh [song ...]       default all seven
#   GAME_DIR (default "C:/GOG Games/Screamer"), OPENSCREAMER (required: the path of an
#   openscreamer checkout); build/fmcheck/ gets the work.
#   SECONDS_RUN=s: how long each song plays (default its length plus 10 s: a full round and more).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
if [ "$1" = "--inner" ]; then
    # inside the container: /w the repository
    shift
    cd /w/build/fmcheck
    gcc -O2 -w -DFM_HOST -I /w/firmware -o fmdump /w/tools/host/fmdump.c /w/firmware/fm.c /w/firmware/dat.c -lm
    gcc -O2 -w -I /w/build/fmcheck/ref -o fmref /w/tools/host/fmref.c
    status=0
    for n in "$@"; do
        read ticks notes < files/MUSIC$n.HMP.notes
        secs=${SECONDS_RUN:-$((ticks / 120 + 10))}
        ./fmdump MUSIC.DAT $n $secs trace > mine.txt 2> ran.txt
        k=$(cat ran.txt)
        ./fmref files $n $k > ref.txt
        # the last tick may not have been finished (the run ended): compare the ticks before it
        awk -v k=$k '$1 < k - 1' mine.txt > mine_cut.txt
        awk -v k=$k '$1 < k - 1' ref.txt > ref_cut.txt
        if cmp -s mine_cut.txt ref_cut.txt; then
            echo "MUSIC$n.HMP: $(wc -l < mine_cut.txt) register writes in $((k - 1)) ticks identical to openscreamer's driver"
        else
            echo "MUSIC$n.HMP: DIFFERENT from openscreamer's driver:"
            diff mine_cut.txt ref_cut.txt | head -10
            status=1
        fi
        ./fmdump MUSIC.DAT $n $secs
    done
    exit $status
fi

game=${GAME_DIR:-C:/GOG Games/Screamer}
os=${OPENSCREAMER:?set OPENSCREAMER to the path of an openscreamer checkout}
# music.c and fmchip.h are copied into build/ (not part of the repository) with an empty port.h
# beside them, so that music.c does not take openscreamer's own (it replaces fopen and the like)
mkdir -p "$root/build/fmcheck/ref"
cp "$os/port/src/music.c" "$os/port/src/fmchip.h" "$root/build/fmcheck/ref/"
: > "$root/build/fmcheck/ref/port.h"
cp "$game/DATA/MUSIC.DAT" "$root/build/fmcheck/MUSIC.DAT"
python -I "$root/tools/fmsongs.py" "$root/build/fmcheck/MUSIC.DAT" "$root/build/fmcheck/files"
songs=${*:-1 2 3 4 5 6 7}
if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$root")
else
    mount=$root
fi
MSYS_NO_PATHCONV=1 docker run --rm -e SECONDS_RUN -v "$mount:/w" --entrypoint sh \
    verilator/verilator:v5.050 /w/tools/fmcheck.sh --inner $songs
