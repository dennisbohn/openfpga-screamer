#!/bin/sh
# Build and run a Verilator testbench in Docker.
# Usage: tools/sim.sh <testbench> [args...]
#   tools/sim.sh soc build/firmware/firmware.bin build/sim/soc
# sim/<testbench>/files.f lists the top module and the Verilog sources, sim/<testbench>/tb.cpp
# drives them; all paths (also in args) are relative to the repository root.
# SIM_DEFINES adds Verilator options, e.g. a fault in a memory model for a negative test:
#   SIM_DEFINES="-DSDRAM_FAULT_DQM_SWAP=1" tools/sim.sh soc ...
# Each set of options gets its own build directory.
# SIM_GAME names a folder inside the repository with the game's original files (all the ones
# core/data.json names: GFX/MAP1H.DAT .. MAP6H.DAT, GAME.DAT, MENU.DAT, GRAF.DAT, DATA/MAP1.DAT
# .. MAP6.DAT, GEN.DAT, LOCAL.DAT, SOUND.DAT, in their folders; build/ is ignored by git, game
# data is never committed). The soc testbench serves them as their data slots, the firmware
# builds the track from them and drives the race (the 3D units, the blitter) instead of the
# texture demo, without the track screen; that takes about 400 frames:
#   SIM_GAME=build/gamedata tools/sim.sh soc build/firmware/firmware.bin build/sim/orig 400
# SIM_QUICK=1 leaves out the two resets after the first run, SIM_IMAGE=<file> offers the
# finished track image as data slot 30 so the firmware does not build it (tools/simcheck.sh).
# SIM_TRACK=<n> races track n (data slot 28; default 1); SIM_IMAGE must then be track n's.
# SIM_MENU=1 shows the track screen instead (data slot 27) and checks its third frame.
# SIM_KEYS=<hex> holds those buttons all the time (cont1_key, firmware/hw.h: 0x20 B the gas,
# 0x80 Y the brake, 0x40 X one press at the start: the far chase); SIM_RACE_FRAMES=<n> runs
# the start lights' countdown at once and checks the n-th frame after the green light, written
# to <prefix>.rgb (python -I tools/rgb2png.py <prefix>.rgb <png>), e.g. the tyre smoke off the
# line, from the far chase, the gas held from the green light, half a second on:
#   SIM_KEYS=60 SIM_RACE_FRAMES=25 tools/simcheck.sh smoke
# SIM_WAV=<file> writes the sound (the I2S output, 48 kHz stereo) of the whole run to a WAV file.
# (blit.v prints its commands with SIM_DEFINES="-DBLIT_TRACE"; with "-DGEOM_STATS" geom.v and
# setup.v print where their clocks went, each frame).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tb=$1
shift
image="verilator/verilator:v5.050"
# the executable is named after the top module, which files.f names
top=$(sed -n 's/^--top-module *//p' "$root/sim/$tb/files.f")
defines=${SIM_DEFINES:-}
obj=obj_$tb
if [ -n "$defines" ]; then
    obj=obj_${tb}_$(printf '%s' "$defines" | tr -c 'A-Za-z0-9' '_')
fi

if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$root")
else
    mount=$root
fi
mkdir -p "$root/build/sim"

MSYS_NO_PATHCONV=1 docker run --rm -e SIM_GAME -e SIM_QUICK -e SIM_IMAGE -e SIM_KEYS -e SIM_RACE_FRAMES -e SIM_TRACK -e SIM_MENU -e SIM_WAV -v "$mount:/work" -w /work --entrypoint sh "$image" -c "
    verilator --cc --exe --build -j 0 -O3 --timescale 1ns/1ps -Wall -Wno-UNUSEDSIGNAL -Wno-DECLFILENAME         $defines --Mdir build/sim/$obj -f sim/$tb/files.f sim/$tb/tb.cpp \
        > build/sim/$obj.build.log 2>&1 || { cat build/sim/$obj.build.log; exit 1; }
    build/sim/$obj/V$top $*
"
