#!/bin/sh
# Build the CPU firmware (firmware/) in Docker with the pinned toolchain of firmware/docker/
# (image screamer-fw:trixie-20261005, built on first use) and copy firmware.bin to
# build/assets/, from where tools/package.sh puts it on the SD card.
# Usage: tools/fw.sh
# Output: build/firmware/firmware.elf, .bin, .lst (disassembly), .map
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
image="screamer-fw:trixie-20261005"
limit=16384                         # boot RAM: firmware.bin may have at most 16 KB

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

MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/work" -w /work/firmware "$image" \
    make --no-print-directory OUT=../build/firmware info

bin="$root/build/firmware/firmware.bin"
size=$(wc -c < "$bin" | tr -d ' ')
if [ "$size" -gt "$limit" ]; then
    echo "firmware.bin has $size bytes, more than the $limit bytes of the boot RAM"
    exit 1
fi
assets="$root/build/assets/screamer/Chainy.Screamer"
mkdir -p "$assets"
cp "$bin" "$assets/firmware.bin"
echo "boot RAM: $size of $limit bytes, copied to build/assets/screamer/Chainy.Screamer/firmware.bin"
