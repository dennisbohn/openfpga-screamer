#!/bin/sh
# Build and run a Verilator testbench in Docker.
# Usage: tools/sim.sh <testbench> [args...]
#   tools/sim.sh fb_video build/assets/screamer/Chainy.Screamer/screen.bin build/sim/fb_video.rgb
# The testbench lives in sim/<testbench>/tb.cpp, the RTL in core/src/fpga/core/<testbench>.v;
# paths in args are relative to the repository root.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tb=$1
shift
image="verilator/verilator:v5.050"

if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$root")
else
    mount=$root
fi
mkdir -p "$root/build/sim"

MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/work" -w /work --entrypoint sh "$image" -c "
    verilator --cc --exe --build -O3 -Wall -Wno-UNUSEDSIGNAL \
        --Mdir build/sim/obj_$tb --top-module $tb \
        core/src/fpga/core/$tb.v sim/$tb/tb.cpp > build/sim/$tb.build.log 2>&1 \
        || { cat build/sim/$tb.build.log; exit 1; }
    build/sim/obj_$tb/V$tb $*
"
