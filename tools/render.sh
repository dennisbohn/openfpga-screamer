#!/bin/sh
# One frame of the race from the firmware's code on the PC (tools/host/render.c), for
# tools/cmpframe.py: seconds instead of a simulation's minutes.
# Usage: tools/render.sh <out.rgb> [view [steps]]     view 2 near chase (default), 3 far chase
#   TRACK=n: track n (default 1). Its image is build/check/trackn.bin, made here by
#   tools/make_track.py if it is missing; the GFX and DATA containers from $GAME_DIR (default
#   "C:/GOG Games/Screamer").
#   MENU=1: the track screen instead, track TRACK chosen, [view] taken as steps of 1/50 s after
#   entering it (the models turn).
# The firmware's sources are copied to build/host/ with hw.h's RISC-V instructions made plain C;
# built and run with the simulation's Docker image (gcc).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
game=${GAME_DIR:-C:/GOG Games/Screamer}
n=${TRACK:-1}
[ $# -ge 1 ] || { echo "usage: [TRACK=n] [MENU=1] tools/render.sh <out.rgb> [view [steps]]"; exit 2; }
src="$root/build/host/src"
mkdir -p "$src"
cp "$root"/firmware/*.c "$root"/firmware/*.h "$root/tools/host/render.c" "$src/"
# hw.h: everything up to the cache maintenance, then the same helpers in plain C
python -I - "$src/hw.h" <<'EOF'
import sys
p = sys.argv[1]
t = open(p).read()
t = t[:t.index("/* Cache maintenance")] + """static inline void dcache_invalidate(void) {}
static inline void icache_sync(void) {}
#define csr_read(csr)   0u
static inline uint32_t rdcycle(void) { return 0; }
static inline int32_t fixmul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static inline uint32_t opaque(uint32_t x) { return x; }
static inline float opaquef(float x) { return x; }
"""
open(p, "w").write(t)
EOF
if [ -n "$MENU" ]; then
    cp "$game/GFX/MENU.DAT" "$game/GFX/GRAF.DAT" "$game/DATA/LOCAL.DAT" "$root/build/host/"
    run="./render menu MENU.DAT GRAF.DAT LOCAL.DAT"
    args="$n ${2:-0}"
else
    if [ ! -f "$root/build/check/track$n.bin" ]; then
        mkdir -p "$root/build/check"
        python -I "$root/tools/make_track.py" "$game/GFX/MAP${n}H.DAT" "$game/DATA/MAP$n.DAT" "$game/DATA/GEN.DAT" \
            "$root/build/check/track$n.bin" "$root/build/check/track$n.png" > /dev/null
    fi
    cp "$root/build/check/track$n.bin" "$root/build/host/track$n.bin"
    cp "$game/GFX/MAP${n}H.DAT" "$game/DATA/MAP$n.DAT" "$root/build/host/"
    run="./render track$n.bin MAP${n}H.DAT MAP$n.DAT"
    args="${2:-2} ${3:-2} $n"
fi
if command -v cygpath >/dev/null 2>&1; then mount=$(cygpath -wa "$root/build"); else mount=$root/build; fi
out=$(basename "$1")
MSYS_NO_PATHCONV=1 docker run --rm ${CAM:+-e CAM} -v "$mount:/b" --entrypoint sh verilator/verilator:v5.050 -c "
    cd /b/host/src && gcc -O2 -w ${DUMP:+-DTRACK_DUMP} -o ../render render.c track.c geom.c setup.c raster.c car.c drive.c opp.c \
        efx.c scenery.c sound.c blit.c dlog.c crc32.c dat.c console.c menu.c tb_hud.c -lm && cd /b/host && $run $out $args"
mv "$root/build/host/$out" "$1"
