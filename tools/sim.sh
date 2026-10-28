#!/bin/sh
# Build and run a Verilator testbench in Docker.
# Usage: tools/sim.sh <testbench> [args...]
#   tools/sim.sh soc build/firmware/firmware.bin build/sim/soc
# sim/<testbench>/files.f lists the top module and the Verilog sources, sim/<testbench>/tb.cpp
# drives them; all paths (also in args) are relative to the repository root.
# SIM_DEFINES adds Verilator options, e.g. a fault in a memory model for a negative test:
#   SIM_DEFINES="-DSDRAM_FAULT_DQM_SWAP=1" tools/sim.sh soc ...
# Each set of options gets its own build directory.
# SIM_GAME names a folder inside the repository with the game's original files (the ones
# core/data.json names: GFX/MAP1H.DAT, GFX/GAME.DAT, DATA/MAP1.DAT, DATA/GEN.DAT,
# DATA/LOCAL.DAT, in their folders; build/ is ignored by git, game data is never committed). The
# soc testbench serves them as their data slots, the firmware builds the track from them
# and drives the race (the 3D units, the blitter) instead of the texture demo; that takes
# about 400 frames:
#   SIM_GAME=build/gamedata tools/sim.sh soc build/firmware/firmware.bin build/sim/orig 400
# SIM_QUICK=1 leaves out the two resets after the first run, SIM_IMAGE=<file> offers the
# finished track image as data slot 30 so the firmware does not build it (tools/simcheck.sh).
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

MSYS_NO_PATHCONV=1 docker run --rm -e SIM_GAME -e SIM_QUICK -e SIM_IMAGE -v "$mount:/work" -w /work --entrypoint sh "$image" -c "
    verilator --cc --exe --build -O3 --timescale 1ns/1ps -Wall -Wno-UNUSEDSIGNAL -Wno-DECLFILENAME \
        $defines --Mdir build/sim/$obj -f sim/$tb/files.f sim/$tb/tb.cpp \
        > build/sim/$obj.build.log 2>&1 || { cat build/sim/$obj.build.log; exit 1; }
    build/sim/$obj/V$top $*
"
