#!/bin/sh
# Run the SoC simulation with the game's files and print a short summary (tools/simsum.py).
# Usage: [SIM_TRACK=n] tools/simcheck.sh [--full] [--save] [name]
#   quick (default): no resets (SIM_QUICK) and the finished track image as a testbench slot
#   (SIM_IMAGE, build/check/trackn.bin, made once by tools/make_track.py), so the simulated CPU
#   does not build it: a few minutes. --full: as on the Pocket, the firmware builds the image,
#   then the two resets; before a release or when the image build or resets changed.
#   SIM_TRACK=n: track n (default 1; the simulation has no track screen). SIM_MENU=1: the
#   track screen instead of the race, its third frame checked (with SIM_TRACK chosen); SIM_MENU=2:
#   the car screen (the first car chosen).
#   --save: this run becomes the baseline the summary compares with. The log goes to
#   build/sim/<name>.log (name default "check"), the sound of the run to build/sim/audio.wav
#   (SIM_WAV names another file).
# Needs build/firmware/ (tools/fw.sh) and all the game's files core/data.json names in
# build/gamedata/ (SIM_GAME), in their folders as on the SD card: build/gamedata/GFX/MAP1H.DAT,
# build/gamedata/DATA/MAP1.DAT, ..., and GOG's CD image at the top, build/gamedata/SCREAM.gog
# (a hard link to it will do) and SCREAM.DAT (the testbench drives the race only when all are
# there).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
quick=1; save=""; name=check
n=${SIM_TRACK:-1}
for a in "$@"; do
    case "$a" in
        --full) quick="" ;;
        --save) save=--save ;;
        *) name=$a ;;
    esac
done
cd "$root"
[ -f build/gamedata/GFX/MAP${n}H.DAT ] || { echo "build/gamedata/ has no game files (see the comment above)"; exit 2; }
image=""
if [ -n "$quick" ]; then
    image=build/check/track$n.bin
    if [ ! -f "$image" ]; then
        mkdir -p build/check
        python -I tools/make_track.py build/gamedata/GFX/MAP${n}H.DAT build/gamedata/DATA/MAP$n.DAT \
            build/gamedata/DATA/GEN.DAT "$image" build/check/track$n.png > /dev/null
    fi
fi
start=$(date +%s)
rc=0
SIM_QUICK=$quick SIM_IMAGE=$image SIM_TRACK=$n SIM_GAME=${SIM_GAME:-build/gamedata} SIM_WAV=${SIM_WAV:-build/sim/audio.wav} \
    sh tools/sim.sh soc \
    build/firmware/firmware.bin "build/sim/$name" 400 > "build/sim/$name.log" 2>&1 || rc=$?
echo "simulation ${quick:+(quick) }exit $rc after $(( $(date +%s) - start )) s, log build/sim/$name.log"
python -I tools/simsum.py "build/sim/$name.log" $save
exit $rc
