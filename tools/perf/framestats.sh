#!/bin/sh
# Frame statistics from the firmware's own C models in Docker (docs/development.md, "Frame statistics").
#   tools/perf/framestats.sh build [dir]
#       copies firmware/ (or the source tree dir/firmware, e.g. an older commit made with git archive) to
#       build/stats/src[_name], puts the counters of perf_patch.py into the copies and builds framestats
#       and racestats there. TAG=name builds into src_name / framestats_name (default: no tag);
#       DEFS="-DLOD_SCALE=1.5f ..." are compile-time options for the firmware's constants
#   tools/perf/framestats.sh sweep <track image> <label> <out.csv> <step> <cars,...> [grid]
#       the frames along the road, see framestats.c (FARSCALE, LODSCALE in the environment; TAG=name)
#   tools/perf/framestats.sh walk <track image> <camera lines file>
#       the cell window and the placements in reach for cameras "x height z yaw" (walkcheck.c), written to <file>.result
#   tools/perf/framestats.sh drop <track image> <lines> <cameras file> [radius]
#       why a placement is drawn from one camera and not from the next (dropcheck.c): cameras "x y h yaw pitch" (the marks of
#       debug.txt), lines the screen's height (132 in the cockpit view, else 200); prints the placements whose state differs
#   tools/perf/framestats.sh race <track n> <label> <out.csv> <steps> <every> [car]
#       a race on the PC with a robot at the keys, a frame every <every> steps, see racestats.c
# The firmware is never changed; the counters live in the copies. Game data only under build/.
# The game's folder is read for the track image (build/check/track<n>.bin, made by tools/make_track.py) and for
# the race (GFX/MAP<n>H.DAT, DATA/MAP<n>.DAT): GAME_DIR, default "C:/GOG Games/Screamer".
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
st="$root/build/stats"
tag=${TAG:+_$TAG}
game=${GAME_DIR:-C:/GOG Games/Screamer}
if command -v cygpath >/dev/null 2>&1; then mount=$(cygpath -wa "$root/build"); else mount=$root/build; fi
img=verilator/verilator:v5.050
docker_sh() { MSYS_NO_PATHCONV=1 docker run --rm "$@"; }
LIST="geom.c setup.c raster.c car.c drive.c opp.c ghost.c efx.c scenery.c sound.c speech.c blit.c dlog.c crc32.c dat.c console.c menu.c champ.c intro.c tb_hud.c tb_spl.c tb_mmp.c hud.c"

cmd=$1
shift
case "$cmd" in
build)
    from=${1:-$root}
    src="$st/src$tag"
    rm -rf "$src"
    mkdir -p "$src"
    cp "$from"/firmware/*.c "$from"/firmware/*.h "$root"/tools/perf/perf.h "$root"/tools/perf/perf.c \
       "$root"/tools/perf/framestats.c "$root"/tools/perf/racestats.c "$root"/tools/perf/walkcheck.c "$root"/tools/perf/dropcheck.c "$src/"
    python -I - "$src/hw.h" <<'PY'
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
PY
    python -I "$root/tools/perf/perf_patch.py" "$src" $REF
    docker_sh -v "$mount:/b" --entrypoint sh $img -c "
        cd /b/stats/src$tag && gcc -O2 -w -include perf.h -DRS_TRACE $DEFS -o ../framestats$tag framestats.c perf.c track.c $LIST -lm &&
        gcc -O2 -w -include perf.h -DRS_TRACE $DEFS -o ../racestats$tag racestats.c perf.c track.c $LIST -lm &&
        gcc -O2 -w -include perf.h -DRS_TRACE $DEFS -o ../walkcheck$tag walkcheck.c perf.c $LIST -lm &&
        gcc -O2 -w -include perf.h -DRS_TRACE $DEFS -o ../dropcheck$tag dropcheck.c perf.c $LIST -lm"
    ;;
sweep)
    image=$1; label=$2; out=$3; step=$4; cars=$5; grid=$6
    cp "$image" "$st/$(basename "$image")"
    docker_sh ${FARSCALE:+-e FARSCALE=$FARSCALE} ${LODSCALE:+-e LODSCALE=$LODSCALE} ${POPIN:+-e POPIN=$POPIN} ${POPIN_DUMP:+-e POPIN_DUMP=$POPIN_DUMP} -v "$mount:/b" --entrypoint sh $img -c \
        "cd /b/stats && ./framestats$tag $(basename "$image") $label $(basename "$out") $step $cars $grid"
    mv "$st/$(basename "$out")" "$out"
    ;;
drop)
    image=$1; lines=$2; cams=$3; radius=$4
    cp "$image" "$st/$(basename "$image")"
    docker_sh -i ${DROP_ALL:+-e DROP_ALL=1} ${DROP_ROWS:+-e DROP_ROWS=1} ${DROP_DRAW:+-e DROP_DRAW=1} -v "$mount:/b" --entrypoint sh $img -c "cd /b/stats && ./dropcheck$tag $(basename "$image") $lines $radius" < "$cams"
    ;;
walk)
    image=$1; out=$2
    cp "$image" "$st/$(basename "$image")"
    docker_sh -i -v "$mount:/b" --entrypoint sh $img -c "cd /b/stats && ./walkcheck$tag $(basename "$image")" < "$out" > "$out.result"
    ;;
race)
    n=$1; label=$2; out=$3; steps=$4; every=$5; car=${6:-0}
    mkdir -p "$st/data"
    cp "$root/build/check/track$n.bin" "$st/data/track$n.bin"
    cp "$game/GFX/MAP${n}H.DAT" "$game/DATA/MAP$n.DAT" "$st/data/"
    docker_sh ${FARSCALE:+-e FARSCALE=$FARSCALE} ${LODSCALE:+-e LODSCALE=$LODSCALE} -v "$mount:/b" --entrypoint sh $img -c \
        "cd /b/stats && ./racestats$tag data/track$n.bin data/MAP${n}H.DAT data/MAP$n.DAT $label $(basename "$out") $steps $every $n $car"
    mv "$st/$(basename "$out")" "$out"
    ;;
*)
    echo "usage: framestats.sh build [dir] | sweep <image> <label> <out.csv> <step> <cars,...> [grid] | race <n> <label> <out.csv> <steps> <every> [car]"
    exit 2
    ;;
esac
