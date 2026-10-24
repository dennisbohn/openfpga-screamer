#!/bin/sh
# Build and run a Verilator testbench in Docker.
# Usage: tools/sim.sh <testbench> [args...]
#   tools/sim.sh fb_system build/assets/screamer/Chainy.Screamer/screen.bin build/sim/fb_system.rgb
# sim/<testbench>/files.f lists the top module and the Verilog sources, sim/<testbench>/tb.cpp
# drives them; all paths (also in args) are relative to the repository root.
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
    verilator --cc --exe --build -O3 -Wall -Wno-UNUSEDSIGNAL -Wno-DECLFILENAME \
        --Mdir build/sim/obj_$tb -f sim/$tb/files.f sim/$tb/tb.cpp \
        > build/sim/$tb.build.log 2>&1 || { cat build/sim/$tb.build.log; exit 1; }
    build/sim/obj_$tb/V$tb $*
"
