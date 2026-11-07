#!/bin/sh
# Runs the matrix of the draw-distance study (docs/development.md, "Draw distance and level of detail").
# Every track gets a race of the robot (a frame every 10 steps, the player and all the opponents
# driving, 600 frames) at each combination of
#   the far plane's factor FARSCALE   1.0, 1.25, 1.5
#   the level of detail's factor LODSCALE   0 (none), 1.0, 1.25, 1.5
# One CSV per combination and track goes to build/stats/study/; tools/perf/study.py makes the
# tables from them.
# Usage: tools/perf/study_sweeps.sh [parallel runs]       (default 3; about 10 minutes)
# Needs: Docker, build/check/track<n>.bin, and the game's folder (GAME_DIR). The script builds the
# counting copy of this tree itself (tools/perf/framestats.sh build).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
j=${1:-3}
out=build/stats/study
mkdir -p $out
sh tools/perf/framestats.sh build > /dev/null
# List all combinations "far factor, level of detail factor, track" and run them j at a time.
for fs in 1.0 1.25 1.5; do
    for ls in 0 1.0 1.25 1.5; do
        for n in 1 2 3 4 5 6; do
            echo "$fs $ls $n"
        done
    done
done | xargs -P "$j" -L 1 sh -c 'FARSCALE=$0 LODSCALE=$1 sh tools/perf/framestats.sh race $2 r$2 build/stats/study/r_$0_$1_t$2.csv 6000 10 > /dev/null 2>&1 || echo "failed: $0 $1 $2"'
# Print the number of files in the result directory.
ls $out | wc -l
