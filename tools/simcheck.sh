#!/bin/sh
# Run the SoC simulation with the game's files and print a short summary (tools/simsum.py).
# Usage: tools/simcheck.sh [--full] [--save] [name]
#   quick (default): no resets (SIM_QUICK) and the finished track image as a testbench slot
#   (SIM_IMAGE, build/check/track1.bin, made once by tools/make_track.py), so the simulated CPU
#   does not build it: a few minutes. --full: as on the Pocket, the firmware builds the image,
#   then the two resets; before a release or when the image build or resets changed.
#   --save: this run becomes the baseline the summary compares with. The log goes to
#   build/sim/<name>.log (name default "check").
# Needs build/firmware/ (tools/fw.sh) and the game's files in build/gamedata/ (SIM_GAME), in
# their folders as on the SD card: build/gamedata/GFX/MAP1H.DAT, build/gamedata/DATA/MAP1.DAT, ...
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
quick=1; save=""; name=check
for a in "$@"; do
    case "$a" in
        --full) quick="" ;;
        --save) save=--save ;;
        *) name=$a ;;
    esac
done
cd "$root"
[ -f build/gamedata/GFX/MAP1H.DAT ] || { echo "build/gamedata/ has no game files (see the comment above)"; exit 2; }
image=""
if [ -n "$quick" ]; then
    image=build/check/track1.bin
    if [ ! -f "$image" ]; then
        mkdir -p build/check
        python -I tools/make_track.py build/gamedata/GFX/MAP1H.DAT build/gamedata/DATA/MAP1.DAT \
            build/gamedata/DATA/GEN.DAT "$image" build/check/track1.png > /dev/null
    fi
fi
start=$(date +%s)
rc=0
SIM_QUICK=$quick SIM_IMAGE=$image SIM_GAME=${SIM_GAME:-build/gamedata} sh tools/sim.sh soc \
    build/firmware/firmware.bin "build/sim/$name" 400 > "build/sim/$name.log" 2>&1 || rc=$?
echo "simulation ${quick:+(quick) }exit $rc after $(( $(date +%s) - start )) s, log build/sim/$name.log"
python -I tools/simsum.py "build/sim/$name.log" $save
exit $rc
