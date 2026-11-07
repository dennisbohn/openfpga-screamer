#!/bin/sh
# Runs the SoC simulation (tools/sim.sh soc) with the game's files and prints a short summary
# (tools/simsum.py).
# Usage: [SIM_TRACK=n] tools/simcheck.sh [--full] [--save] [name]
#   name      name of the run: the log goes to build/sim/<name>.log (default "check")
#   --full    as on the Pocket: the firmware builds the track image, then the two resets follow.
#             Use it before a release and when the image build or the resets changed.
#   --save    this run becomes the baseline that the summary compares with
# Without --full the run is quick: there are no resets (SIM_QUICK) and the finished track image is
# offered as a testbench slot (SIM_IMAGE, build/check/track<n>.bin, made once by tools/make_track.py).
# The simulated CPU then does not build the image, which is much faster.
#
# Environment (the options of tools/sim.sh that are not listed here pass through to it):
#   SIM_TRACK=n       the track to race (default 1). The simulation has no track screen.
#   SIM_MENU=1        show the track screen instead of the race and check its third frame
#                     (the track is the one SIM_TRACK names). SIM_MENU=2: the car screen, the
#                     first car chosen.
#   SIM_FW=dir        the firmware folder to run (default build/firmware-debug, made by
#                     tools/fw.sh DEBUG=1; the log lines the summary reads come from that build).
#                     Another folder, for example a copy of a build to compare with, works as well.
#   SIM_FRAMES=n      the simulation's timeout in frames (default 400). --full on track 6 needs
#                     more: its image takes 4.4 s to build.
#   SIM_GAME=dir      the folder with the game's files (default build/gamedata)
#   SIM_WAV=file      the sound of the run (default build/sim/audio.wav)
#
# Needs: Docker, the firmware in build/firmware-debug/ (tools/fw.sh DEBUG=1) and all the game's
# files that core/data.json names in build/gamedata/, in their folders as on the SD card:
# build/gamedata/GFX/MAP1H.DAT, build/gamedata/DATA/MAP1.DAT, ... and GOG's CD image at the top,
# build/gamedata/SCREAM.gog (a hard link to it will do) and SCREAM.DAT. The testbench drives the
# race only when all of them are there.
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
# Check that the inputs exist.
[ -f build/gamedata/GFX/MAP${n}H.DAT ] || { echo "build/gamedata/ has no game files (see the comment above)"; exit 2; }
[ -f "${SIM_FW:-build/firmware-debug}/firmware.bin" ] || { echo "no firmware in ${SIM_FW:-build/firmware-debug}: tools/fw.sh DEBUG=1 builds it"; exit 2; }
# Quick run: the finished track image is made once with the reference script and kept.
image=""
if [ -n "$quick" ]; then
    image=build/check/track$n.bin
    if [ ! -f "$image" ]; then
        mkdir -p build/check
        python -I tools/make_track.py build/gamedata/GFX/MAP${n}H.DAT build/gamedata/DATA/MAP$n.DAT \
            build/gamedata/DATA/GEN.DAT "$image" build/check/track$n.png > /dev/null
    fi
fi
# Simulate (the log is the output) and print the summary of the log.
start=$(date +%s)
rc=0
SIM_QUICK=$quick SIM_IMAGE=$image SIM_TRACK=$n SIM_GAME=${SIM_GAME:-build/gamedata} SIM_WAV=${SIM_WAV:-build/sim/audio.wav} \
    sh tools/sim.sh soc \
    "${SIM_FW:-build/firmware-debug}/firmware.bin" "build/sim/$name" "${SIM_FRAMES:-400}" > "build/sim/$name.log" 2>&1 || rc=$?
echo "simulation ${quick:+(quick) }exit $rc after $(( $(date +%s) - start )) s, log build/sim/$name.log"
python -I tools/simsum.py "build/sim/$name.log" $save
exit $rc
