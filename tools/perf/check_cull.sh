#!/bin/sh
# The cull must hand the same pieces to the geometry unit, in the same order, as before a change of it.
# Counting copies of the firmware of a reference commit and of this tree (tools/perf/framestats.sh build,
# perf_patch.py: a hash of the sorted list of pieces, and the pieces, faces, triangles and vertices counted by
# the C models) draw the same frames: the road sweeps with 0, 1 and 4 cars (160 frames a track) and a robot's
# race of each track; every frame's hash and counters must be equal.
# Usage: tools/perf/check_cull.sh <reference commit> [tracks]       (default tracks 1 2 3 4 5 6)
# Needs: build/check/track<n>.bin (tools/make_track.py, made by tools/simcheck.sh or tools/render.sh), the game's folder
# (GAME_DIR) for the races. About 3 minutes.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
ref=$1
shift
tracks=${*:-1 2 3 4 5 6}
[ -n "$ref" ] || { echo "usage: check_cull.sh <reference commit> [tracks]"; exit 2; }
cd "$root"
old=build/ref_tree
mkdir -p build/stats/out
rm -rf "$old"
mkdir -p "$old"
git archive "$ref" | tar -x -C "$old"
TAG=ref REF=--ref sh tools/perf/framestats.sh build "$root/$old" > /dev/null
sh tools/perf/framestats.sh build > /dev/null
bad=0
for n in $tracks; do
    img=build/check/track$n.bin
    TAG=ref sh tools/perf/framestats.sh sweep "$img" s$n build/stats/out/ref_sweep$n.csv -160 0,1,4 > /dev/null 2>&1
    sh tools/perf/framestats.sh sweep "$img" s$n build/stats/out/new_sweep$n.csv -160 0,1,4 > /dev/null 2>&1
    TAG=ref sh tools/perf/framestats.sh race $n r$n build/stats/out/ref_race$n.csv 6000 10 > /dev/null 2>&1
    sh tools/perf/framestats.sh race $n r$n build/stats/out/new_race$n.csv 6000 10 > /dev/null 2>&1
    python -I - "$n" build/stats/out <<'PY' || bad=$((bad + 1))
import csv, sys
n, d = sys.argv[1:3]
cols = ("vis_hash", "ts_pieces", "ts_faces", "ts_triangles", "ts_vertices", "ts_objects", "gu_pieces", "faces_row", "tris_tex", "px_drawn")
fail = 0
for kind in ("sweep", "race"):
    ra = list(csv.DictReader(open(f"{d}/ref_{kind}{n}.csv")))
    rb = list(csv.DictReader(open(f"{d}/new_{kind}{n}.csv")))
    diff = sum(1 for x, y in zip(ra, rb) if any(x[c] != y[c] for c in cols))
    mean = lambda r, c: sum(float(x[c]) for x in r) / max(1, len(r))
    print(f"track {n} {kind:5}: {len(ra)} / {len(rb)} frames, {diff} differ; placements tested {mean(ra, 'tested'):.0f} -> {mean(rb, 'tested'):.0f}")
    fail += diff + (len(ra) != len(rb)) + (len(ra) == 0)
sys.exit(1 if fail else 0)
PY
done
if [ $bad -ne 0 ]; then echo "FAIL: the cull changed the pieces drawn on $bad track(s)"; exit 1; fi
echo "ok: the same pieces in the same order in every frame"
