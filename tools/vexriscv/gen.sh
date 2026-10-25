#!/bin/sh
# Generate the VexRiscv CPUs of tools/vexriscv/PocketVex.scala (im, imf, imfc) as Verilog with
# sbt in Docker and copy the core's CPU to core/src/fpga/core/cpu/VexPocketIMFC.v.
# Usage: tools/vexriscv/gen.sh
# The first run clones VexRiscv at the pinned commit into build/vexriscv/src and downloads sbt,
# Scala and SpinalHDL into build/vexriscv/cache; later runs work offline.
# Output: build/vexriscv/out/<variant>/<Module>.v (+ .yaml), full log in build/vexriscv/gen.log.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
work="$root/build/vexriscv"
repo="https://github.com/SpinalHDL/VexRiscv.git"
commit="baf7dc82f855eddaf5b0a20a6802c526f127e38a"
# JDK 11 like VexRiscv's CI, Scala 2.12.18 like its build.sbt; the sbt 1.9.9 launcher in the
# image fetches sbt 1.6.0 (project/build.properties). SpinalHDL 1.13.0 comes from build.sbt.
image="sbtscala/scala-sbt:eclipse-temurin-jammy-11.0.22_7_1.9.9_2.12.18@sha256:01e388f79dae4899a1ce169eb2d8792ccb718442051b02937da7416d89724e70"

# Git Bash on Windows: hand Docker Windows paths and stop MSYS rewriting the container paths.
w() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi; }

src="$work/src"
if [ ! -d "$src/.git" ]; then
    echo "Cloning VexRiscv into $src"
    # LF line endings like on Linux, whatever core.autocrlf says
    git -c core.longpaths=true clone --quiet --config core.longpaths=true \
        --config core.autocrlf=false "$repo" "$src"
fi
git -C "$src" cat-file -e "$commit^{commit}" 2>/dev/null || git -C "$src" fetch --quiet origin
git -c core.longpaths=true -C "$src" checkout --quiet --detach "$commit"
[ -z "$(git -C "$src" status --porcelain)" ] || { echo "$src has local changes"; exit 1; }

rm -rf "$work/out"
mkdir -p "$work/out" "$work/cache/coursier" "$work/cache/ivy2" "$work/cache/sbt"
log="$work/gen.log"
[ -n "$(ls -A "$work/cache/sbt")" ] || echo "First run: sbt downloads itself, Scala and SpinalHDL (about 250 MB)"
echo "Running sbt in Docker, log: $log"
start=$(date +%s)
if ! MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$(w "$src"):/src:ro" \
    -v "$(w "$root/tools/vexriscv"):/tools:ro" \
    -v "$(w "$work/out"):/out" \
    -v "$(w "$work/cache/coursier"):/root/.cache/coursier" \
    -v "$(w "$work/cache/ivy2"):/root/.ivy2" \
    -v "$(w "$work/cache/sbt"):/root/.sbt" \
    "$image" sh /tools/gen_in_container.sh > "$log" 2>&1; then
    tail -20 "$log"
    echo "Generation failed, full log: $log"
    exit 1
fi
echo "Generated in $(( $(date +%s) - start )) s:"
(cd "$work/out" && find . -name "*.v" | sed 's|^\./|  build/vexriscv/out/|' | sort)

cpu="$root/core/src/fpga/core/cpu/VexPocketIMFC.v"
if cmp -s "$work/out/imfc/VexPocketIMFC.v" "$cpu"; then
    echo "core/src/fpga/core/cpu/VexPocketIMFC.v unchanged"
else
    mkdir -p "$(dirname "$cpu")"
    cp "$work/out/imfc/VexPocketIMFC.v" "$cpu"
    echo "core/src/fpga/core/cpu/VexPocketIMFC.v updated"
fi
