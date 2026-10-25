#!/bin/sh
# Runs inside the sbt image, started by tools/vexriscv/gen.sh: copies what sbt needs from the
# read-only VexRiscv clone (/src) to /tmp/vex, adds PocketVex.scala as package "pocket" and
# generates the given variants (none: all) into /out/<variant>/.
set -e
mkdir -p /tmp/vex/src
cp -r /src/build.sbt /src/project /tmp/vex/
cp -r /src/src/main /tmp/vex/src/
mkdir -p /tmp/vex/src/main/scala/pocket
cp /tools/PocketVex.scala /tmp/vex/src/main/scala/pocket/
cd /tmp/vex
java -version 2>&1 | head -1
sbt -batch -Dsbt.server.forcestart=false "runMain pocket.PocketVexGen /out $*"
