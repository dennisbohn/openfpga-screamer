#!/bin/sh
# Disassemble a stretch of the game's program, at the addresses openscreamer docs/NOTES.md uses.
# Usage: tools/dis.sh [-h] [-f] <from> <to>     e.g. tools/dis.sh 0x8a598 0x8a628
#   -h  STARTH.EXE (the SVGA version) instead of STARTL.EXE
#   -f  the image with the LE fixups applied (pointers are flat addresses)
# The flat images are made once by tools/unle.py from the game folder ($GAME_DIR, default
# "C:/GOG Games/Screamer") into build/exe/, never into the repository. Prints objdump's lines
# without its header; the object bases are printed by tools/unle.py.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
game=${GAME_DIR:-C:/GOG Games/Screamer}
exe=STARTL; fix=""
while [ $# -gt 2 ]; do
    case "$1" in
        -h) exe=STARTH ;;
        -f) fix=_fix ;;
        *) break ;;
    esac
    shift
done
[ $# -eq 2 ] || { echo "usage: tools/dis.sh [-h] [-f] <from> <to>"; exit 2; }
name=$(echo "$exe" | tr 'A-Z' 'a-z')$fix.bin
mkdir -p "$root/build/exe"
if [ ! -f "$root/build/exe/$name" ]; then
    python -I "$root/tools/unle.py" "$game/$exe.EXE" -o "$root/build/exe/$name" \
        ${fix:+--fixups} > /dev/null
fi
if command -v cygpath >/dev/null 2>&1; then mount=$(cygpath -w "$root/build/exe"); else mount=$root/build/exe; fi
MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/x" --entrypoint sh verilator/verilator:v5.050 -c \
    "objdump -D -b binary -m i386 -M intel --start-address=$1 --stop-address=$2 /x/$name" | tail -n +8
