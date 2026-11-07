#!/bin/sh
# Builds a Verilator testbench in Docker (image verilator/verilator:v5.050) and runs it.
# Usage: tools/sim.sh <testbench> [args...]
#   tools/sim.sh soc build/firmware/firmware.bin build/sim/soc
# sim/<testbench>/files.f lists the top module and the Verilog sources, and sim/<testbench>/tb.cpp
# drives them. All paths, also in args, are relative to the repository root.
# Each set of options gets its own build directory in build/sim/. Runs that start at the same time
# take turns building it (flock), then simulate side by side.
# tools/simcheck.sh is the usual way to call this script for the soc testbench.
#
# Environment:
#   SIM_DEFINES   extra Verilator options, for example a fault in a memory model for a negative test:
#                 SIM_DEFINES="-DSDRAM_FAULT_DQM_SWAP=1" tools/sim.sh soc ...
#                 -DBLIT_TRACE makes blit.v print its commands. -DGEOM_STATS makes geom.v and setup.v
#                 print, each frame, where their clocks went.
#   SIM_GAME      a folder inside the repository with the game's original files, all the ones
#                 core/data.json names: GFX/MAP1H.DAT .. MAP6H.DAT, GAME.DAT, MENU.DAT, GRAF.DAT,
#                 DATA/MAP1.DAT .. MAP6.DAT, GEN.DAT, LOCAL.DAT, SOUND.DAT, in their folders. build/
#                 is ignored by git, so game data is never committed. The soc testbench serves the
#                 files as their data slots. The firmware then builds the track from them and drives
#                 the race (the 3D units, the blitter) instead of listing the missing files. There is
#                 no track screen; the run takes about 400 frames:
#                 SIM_GAME=build/gamedata tools/sim.sh soc build/firmware/firmware.bin build/sim/orig 400
#   SIM_QUICK=1   leaves out the two resets after the first run.
#   SIM_IMAGE     a file with the finished track image, offered as data slot 30 so that the firmware
#                 does not build it (tools/simcheck.sh).
#   SIM_TRACK=n   races track n (data slot 28; default 1). SIM_IMAGE must then be track n's.
#   SIM_MENU=1    shows the track screen instead (data slot 27) and checks its third frame.
#   SIM_MENU=2    shows the car screen.
#   SIM_KEYS=hex  holds those buttons all the time (cont1_key in firmware/hw.h): 0x20 is B, the gas,
#                 0x80 is Y, the brake, and 0x40 is X, pressed once at the start to switch to the
#                 far chase camera.
#   SIM_RACE_FRAMES=n
#                 runs the start lights' countdown at once and checks the n-th frame after the green
#                 light. The frame is written to <prefix>.rgb, which tools/rgb2png.py converts:
#                 python -I tools/rgb2png.py <prefix>.rgb <png>. Example, the tyre smoke half a
#                 second after the green light, with the far chase and the gas held:
#                 SIM_KEYS=60 SIM_RACE_FRAMES=25 tools/simcheck.sh smoke
#   SIM_CFG=file  this screamer.cfg (firmware/config.h) instead of the one in SIM_GAME, to run
#                 settings side by side.
#   SIM_WAV=file  writes the sound of the whole run (the I2S output, 48 kHz stereo) to a WAV file.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tb=$1
shift
image="verilator/verilator:v5.050"
# Verilator names the executable after the top module, which files.f gives.
top=$(sed -n 's/^--top-module *//p' "$root/sim/$tb/files.f")
defines=${SIM_DEFINES:-}
obj=obj_$tb
if [ -n "$defines" ]; then
    obj=obj_${tb}_$(printf '%s' "$defines" | tr -c 'A-Za-z0-9' '_')
fi

# Git Bash on Windows: hand Docker a Windows path.
if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$root")
else
    mount=$root
fi
mkdir -p "$root/build/sim"

# In the container: build the testbench (one build directory per option set), then run it
# with the arguments of this script. MSYS_NO_PATHCONV stops Git Bash rewriting /work.
MSYS_NO_PATHCONV=1 docker run --rm -e SIM_GAME -e SIM_CFG -e SIM_QUICK -e SIM_IMAGE -e SIM_KEYS -e SIM_RACE_FRAMES -e SIM_TRACK -e SIM_MENU -e SIM_WAV -v "$mount:/work" -w /work --entrypoint sh "$image" -c "
    flock build/sim/$obj.lock verilator --cc --exe --build -j 0 -O3 --timescale 1ns/1ps -Wall -Wno-UNUSEDSIGNAL -Wno-DECLFILENAME \
        $defines --Mdir build/sim/$obj -f sim/$tb/files.f sim/$tb/tb.cpp \
        > build/sim/$obj.build.log 2>&1 || { cat build/sim/$obj.build.log; exit 1; }
    build/sim/$obj/V$top $*
"
