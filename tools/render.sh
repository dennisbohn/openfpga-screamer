#!/bin/sh
# One frame of the race from the firmware's code on the PC (tools/host/render.c), for
# tools/cmpframe.py: seconds instead of a simulation's minutes.
# Usage: tools/render.sh <out.rgb> [view [steps]]     view 2 near chase (default), 3 far chase
#   TRACK=n: track n (default 1). Its image is build/check/trackn.bin, made here by
#   tools/make_track.py if it is missing; the GFX and DATA containers from $GAME_DIR (default
#   "C:/GOG Games/Screamer"). CAR=r: the player's car, row r of CARS.DTR (default 0).
#   MENU=1: the track screen instead, track TRACK chosen, [view] taken as steps of 1/50 s after
#   entering it (the models turn); MENU=2: the car screen, car CAR chosen; MENU=3: track TRACK's
#   loading screen; MENU=4: the main menu, MENU=5: the options page, MENU=6: the championship's
#   stage CHAMP="s[,failed]"; MENU=7: VIEW RECORDS of track TRACK (two made-up races entered),
#   MENU=8: ENTER YOUR NAME; MENUKEYS="DDB...": presses first (U D L R B A, tools/host/render.c).
#   PAUSE=1: the pause's picture over the race's, PAUSE=2: PLAY AGAIN? (tools/host/render.c).
#   CAM="x y h yaw pitch roll": the original's camera record; REFL="x y z [phase]": the player's
#   reflection accumulators and picture from the original's car record (+0x30, +0x3c), the car
#   standing; GAS=1: the pedal down for the steps (the race starts after 300); TURN=k: the last k
#   steps steering left (-k right), BRAKE=k: the last k braking instead. TRACKSIDE=1: the
#   trackside camera of the race's end after the steps (MAPnRPL.SPL); HUD=1: the race screen over it.
#   REPLAYCHECK=s: the replay's determinism check instead (a robot's race of at most s steps, its
#   replay compared step by step; CAR=6 finishes track 1); the picture is the replay's at its
#   end, or at step REPLAYAT, NOHUD=1 without the race screen; REPLAYDUMP=f: the race's steps and
#   frames into build/host/f (tools/host/render.c), to compare two versions with cmp. MODE=m: the
#   game mode (car.h, 4 time attack, 0x20 cone carnage, 0x40 slalom) instead of the normal game;
#   with GHOST=1 and MODE=4 REPLAYCHECK runs two robot races, the second against the first one's
#   ghost.
# The firmware's sources are copied to build/host/ with hw.h's RISC-V instructions made plain C;
# built and run with the simulation's Docker image (gcc).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
game=${GAME_DIR:-C:/GOG Games/Screamer}
n=${TRACK:-1}
[ $# -ge 1 ] || { echo "usage: [TRACK=n] [CAR=r] [MENU=1..5] tools/render.sh <out.rgb> [view [steps]]"; exit 2; }
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
    cp "$game/GFX/MAP${n}H.DAT" "$game/DATA/MAP$n.DAT" "$root/build/host/"
    run="./render track$n.bin MAP${n}H.DAT MAP$n.DAT"
    args="${2:-2} ${3:-2} $n ${CAR:-0}"
fi
if command -v cygpath >/dev/null 2>&1; then mount=$(cygpath -wa "$root/build"); else mount=$root/build; fi
out=$(basename "$1")
MSYS_NO_PATHCONV=1 docker run --rm ${CAM:+-e CAM} ${REFL:+-e REFL} ${GAS:+-e GAS} ${TURN:+-e TURN} ${BRAKE:+-e BRAKE} ${PAUSE:+-e PAUSE} ${MENUKEYS:+-e MENUKEYS} ${CHAMP:+-e CHAMP} \
    ${TRACKSIDE:+-e TRACKSIDE} ${HUD:+-e HUD} ${REPLAYCHECK:+-e REPLAYCHECK} ${REPLAYAT:+-e REPLAYAT} ${NOHUD:+-e NOHUD} ${REPLAYDUMP:+-e REPLAYDUMP} ${MODE:+-e MODE} ${GHOST:+-e GHOST} -v "$mount:/b" --entrypoint sh verilator/verilator:v5.050 -c "
    cd /b/host/src && gcc -O2 -w ${DUMP:+-DTRACK_DUMP} -o ../render render.c track.c geom.c setup.c raster.c car.c drive.c opp.c ghost.c \
        efx.c scenery.c sound.c speech.c blit.c dlog.c crc32.c dat.c console.c menu.c champ.c tb_hud.c tb_spl.c tb_mmp.c hud.c -lm && cd /b/host && $run $out $args"
mv "$root/build/host/$out" "$1"
