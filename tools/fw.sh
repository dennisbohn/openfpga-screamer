#!/bin/sh
# Builds the CPU firmware (firmware/) in Docker with the pinned toolchain of firmware/docker/
# (image screamer-fw:trixie-20261005, built on first use). The result is firmware.bin (the boot
# loader) and program.bin (the program that runs from the SDRAM).
#
# Usage: tools/fw.sh                 the release firmware: none of the measuring and diagnostic code
#                                    (firmware/debug.h). Copied to build/assets/screamer/Chainy.Screamer/,
#                                    from where tools/package.sh puts it on the SD card.
#        tools/fw.sh DEBUG=1         the development firmware in build/firmware-debug/, not copied. It has
#                                    the frame time display, the benchmark, the marks and the free camera,
#                                    the statistics and the timeline. The simulation and the PC tools
#                                    that read the log (tools/simcheck.sh) use it.
#        tools/fw.sh FAULT=IFETCH    a fault test for sim/soc in build/firmware-IFETCH/, not copied.
# Needs: Docker (Git Bash on Windows works).
# Output: build/<dir>/{firmware,program}.elf, .bin, .lst (disassembly), .map, where <dir> is
#         firmware, firmware-debug or firmware-<FAULT>.
# The script stops if firmware.bin is larger than the 32 KB boot RAM or program.bin is larger than
# data slot 2 (1 MB, core/data.json).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
image="screamer-fw:trixie-20261005"
limit=32768                         # boot RAM: firmware.bin may have at most 32 KB
prog_limit=1048576                  # data slot 2 (core/data.json): program.bin

# Git Bash on Windows: hand Docker Windows paths and stop MSYS rewriting /work.
if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$root")
    context=$(cygpath -w "$root/firmware/docker")
else
    mount=$root
    context=$root/firmware/docker
fi

if ! docker image inspect "$image" > /dev/null 2>&1; then
    echo "Building the toolchain image $image (once)"
    MSYS_NO_PATHCONV=1 docker build -t "$image" "$context"
fi

fault=""
debug=""
for arg in "$@"; do
    case "$arg" in
        FAULT=*) fault=${arg#FAULT=} ;;
        DEBUG=*) debug=${arg#DEBUG=}; [ "$debug" = 0 ] && debug="" ;;
    esac
done
out=firmware${fault:+-$fault}${debug:+-debug}

MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/work" -w /work/firmware "$image" \
    make --no-print-directory OUT=../build/$out "$@" info

bin="$root/build/$out/firmware.bin"
prog="$root/build/$out/program.bin"
size=$(wc -c < "$bin" | tr -d ' ')
prog_size=$(wc -c < "$prog" | tr -d ' ')
if [ "$size" -gt "$limit" ]; then
    echo "firmware.bin has $size bytes, more than the $limit bytes of the boot RAM"
    exit 1
fi
if [ "$prog_size" -gt "$prog_limit" ]; then
    echo "program.bin has $prog_size bytes, more than the $prog_limit bytes of data slot 2"
    exit 1
fi
if [ -n "$fault" ]; then
    echo "fault test: build/$out/firmware.bin, $size bytes"
    exit 0
fi
if [ -n "$debug" ]; then
    echo "debug build: build/$out/{firmware,program}.bin, $size and $prog_size bytes (not copied to build/assets: the release is the build without DEBUG)"
    exit 0
fi
assets="$root/build/assets/screamer/Chainy.Screamer"
mkdir -p "$assets"
cp "$bin" "$assets/firmware.bin"
cp "$prog" "$assets/program.bin"
echo "boot RAM: $size of $limit bytes; program.bin: $prog_size bytes; both copied to build/assets/screamer/Chainy.Screamer/"
