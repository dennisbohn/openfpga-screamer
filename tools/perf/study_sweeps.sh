#!/bin/sh
# The matrix of the draw-distance study (docs/development.md, "Draw distance and level of detail"): every track's
# race of the robot (a frame every 10 steps, the player and all the opponents driving, 600 frames) at the far
# plane's factor FARSCALE 1.0 / 1.25 / 1.5 and the level-of-detail's factor LODSCALE 0 (none) / 1.0 / 1.25 / 1.5,
# one CSV a combination and track in build/stats/study/. Then tools/perf/study.py makes the tables.
# Usage: tools/perf/study_sweeps.sh [parallel runs]       (default 3; about 10 minutes)
# Needs: tools/perf/framestats.sh build (the counting copy of this tree), build/check/track<n>.bin, the game's folder.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
j=${1:-3}
out=build/stats/study
mkdir -p $out
sh tools/perf/framestats.sh build > /dev/null
for fs in 1.0 1.25 1.5; do
    for ls in 0 1.0 1.25 1.5; do
        for n in 1 2 3 4 5 6; do
            echo "$fs $ls $n"
        done
    done
done | xargs -P "$j" -L 1 sh -c 'FARSCALE=$0 LODSCALE=$1 sh tools/perf/framestats.sh race $2 r$2 build/stats/study/r_$0_$1_t$2.csv 6000 10 > /dev/null 2>&1 || echo "failed: $0 $1 $2"'
ls $out | wc -l
