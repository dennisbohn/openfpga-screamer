#!/bin/sh
# Draws one frame of the race, or of a menu screen, with the firmware's own code on the PC
# (tools/host/render.c), for tools/cmpframe.py: seconds instead of a simulation's minutes.
# Usage: tools/render.sh <out.rgb> [view [steps]]
#   out.rgb   the picture, raw RGB with 3 bytes per pixel (tools/rgb2png.py converts it to a PNG)
#   view      2 near chase (default), 3 far chase; the other views are listed in tools/host/render.c.
#             With MENU it is the number of steps of 1/50 s after entering the screen (the models turn).
#   steps     physics steps before the picture (default 2); with INTRO=1 the steps of the intro
# Needs: Docker (the simulation's image verilator/verilator:v5.050 supplies gcc) and Python.
# The script copies the firmware's sources to build/host/src with the RISC-V instructions of hw.h
# replaced by plain C, builds tools/host/render.c with them and runs the program in Docker.
#
# Options of this script, as environment variables:
#   TRACK=n    the track (default 1). Its image is build/check/track<n>.bin; tools/make_track.py
#              makes it from the game's files if it is missing. The GFX and DATA containers come
#              from $GAME_DIR (default "C:/GOG Games/Screamer").
#   CAR=r      the player's car, row r of CARS.DTR (default 0)
#   MENU=k     a menu screen instead of the race, track TRACK chosen: 1 the track screen, 2 the car
#              screen with car CAR chosen, 3 track TRACK's loading screen, 4 the main menu, 5 the
#              options page, 6 the championship's stage CHAMP="s[,failed]", 7 VIEW RECORDS of
#              track TRACK (two made-up races entered), 8 ENTER YOUR NAME. The menu is as a new
#              game has it (three tracks, twelve cars).
#   MENUKEYS="DDB..."   key presses before the screen is drawn (U D L R B A, see tools/host/render.c)
#   LEVEL=n    CHOICE.DAT's level (the tracks up to n + 3, NEW TRACK AVAILABLE)
#   BULLET=1   the Bullet was won
#   PAUSE=1    the pause's picture over the race's, CD="b[,p[,c[,v]]]" its cursor on button b,
#              pressed, volume channel c at v; PAUSE=2: PLAY AGAIN?
#   CAM="x y h yaw pitch roll"   the original's camera record
#   REFL="x y z [phase]"         the player's reflection accumulators and picture from the original's
#              car record (+0x30, +0x3c), with the car standing
#   GAS=1      the pedal down for the steps (the race starts after 300 steps)
#   TURN=k     the last k steps steering left (-k: right); BRAKE=k: the last k steps braking instead
#   TRACKSIDE=1   the trackside camera of the race's end after the steps (MAPnRPL.SPL)
#   HUD=1      the race screen over the picture
#   REPLAYCHECK=s   the replay's determinism check instead: a robot's race of at most s steps, its
#              replay compared step by step (CAR=6 finishes track 1). The picture is the replay's
#              at its end, or at step REPLAYAT. NOHUD=1 leaves out the race screen. REPLAYDUMP=f
#              writes the race's steps and frames into build/host/f, to compare two versions
#              with cmp.
#   MODE=m     the game mode (car.h: 4 time attack, 0x20 cone carnage, 0x40 slalom) instead of the
#              normal game. With GHOST=1 and MODE=4, REPLAYCHECK runs two robot races, the second
#              against the first one's ghost.
#   INTRO=1    the intro's picture after the steps (the third argument), the logos and credits
#              over it (run_intro in main.c)
#   DEFS="-DFAR_SCALE=1.0f -DLOD_SCALE=0"   compiler options for the firmware's constants (track.h)
#   DUMP=1     compiles with -DTRACK_DUMP: the program also prints the list of drawn pieces and
#              objects (firmware/track.c)
# The variables from CAM on are read by tools/host/render.c, which describes them in detail.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
game=${GAME_DIR:-C:/GOG Games/Screamer}
n=${TRACK:-1}
[ $# -ge 1 ] || { echo "usage: [TRACK=n] [CAR=r] [MENU=1..5] tools/render.sh <out.rgb> [view [steps]]"; exit 2; }
src="$root/build/host/src"
mkdir -p "$src"
cp "$root"/firmware/*.c "$root"/firmware/*.h "$root/tools/host/render.c" "$src/"
# hw.h: keep everything up to the cache maintenance, then add the same helpers in plain C.
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
    cp "$game/GFX/MENU.DAT" "$game/GFX/GRAF.DAT" "$game/DATA/LOCAL.DAT" "$game/DATA/GEN.DAT" "$root/build/host/"
    run="./render menu MENU.DAT GRAF.DAT LOCAL.DAT GEN.DAT"
    [ "$MENU" = 2 ] && n=$(( ${CAR:-0} + 1 ))
    args="$MENU $n ${2:-0}"
else
    if [ ! -f "$root/build/check/track$n.bin" ]; then
        mkdir -p "$root/build/check"
        python -I "$root/tools/make_track.py" "$game/GFX/MAP${n}H.DAT" "$game/DATA/MAP$n.DAT" "$game/DATA/GEN.DAT" \
            "$root/build/check/track$n.bin" "$root/build/check/track$n.png" > /dev/null
    fi
    cp "$root/build/check/track$n.bin" "$root/build/host/track$n.bin"
    [ -n "$PAUSE" ] && cp "$game/GFX/GAME.DAT" "$root/build/host/"
    [ -n "$INTRO" ] && cp "$game/GFX/GRAF.DAT" "$game/DATA/LOCAL.DAT" "$root/build/host/"
    cp "$game/GFX/MAP${n}H.DAT" "$game/DATA/MAP$n.DAT" "$root/build/host/"
    run="./render track$n.bin MAP${n}H.DAT MAP$n.DAT"
    args="${2:-2} ${3:-2} $n ${CAR:-0}"
fi
# Git Bash on Windows: hand Docker a Windows path.
if command -v cygpath >/dev/null 2>&1; then mount=$(cygpath -wa "$root/build"); else mount=$root/build; fi
# Build the program in the container (gcc) and run it there. The options reach it through -e.
out=$(basename "$1")
MSYS_NO_PATHCONV=1 docker run --rm ${CAM:+-e CAM} ${REFL:+-e REFL} ${GAS:+-e GAS} ${TURN:+-e TURN} ${BRAKE:+-e BRAKE} ${PAUSE:+-e PAUSE} ${CD:+-e CD} ${MENUKEYS:+-e MENUKEYS} ${CHAMP:+-e CHAMP} \
    ${TRACKSIDE:+-e TRACKSIDE} ${HUD:+-e HUD} ${REPLAYCHECK:+-e REPLAYCHECK} ${REPLAYAT:+-e REPLAYAT} ${NOHUD:+-e NOHUD} ${REPLAYDUMP:+-e REPLAYDUMP} ${MODE:+-e MODE} ${GHOST:+-e GHOST} ${INTRO:+-e INTRO} ${LEVEL:+-e LEVEL} ${BULLET:+-e BULLET} -v "$mount:/b" --entrypoint sh verilator/verilator:v5.050 -c "
    cd /b/host/src && gcc -O2 -w ${DUMP:+-DTRACK_DUMP} $DEFS -o ../render render.c track.c geom.c setup.c raster.c car.c drive.c opp.c ghost.c \
        efx.c scenery.c sound.c speech.c blit.c dlog.c crc32.c dat.c console.c menu.c champ.c intro.c tb_hud.c tb_spl.c tb_mmp.c hud.c -lm && cd /b/host && $run $out $args"
# The program wrote the picture to build/host/; move it to where the caller asked.
mv "$root/build/host/$out" "$1"
