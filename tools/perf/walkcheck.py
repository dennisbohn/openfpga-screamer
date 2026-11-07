"""Compares the firmware's cell window and cell table with the original's (docs/original_geometry.md 2.1).

Usage: python -I tools/perf/walkcheck.py <walk json> <track image>

The json is the output of tools/dosram.py walk. For a few cameras of a race in DOSBox it holds the
original's view structure, its cell window (the rows and columns it visits) and the list of pieces in
reach (original: STARTL.EXE 0x83164), in the original's order.
The script runs the firmware's own track.c (tools/perf/walkcheck.c, built and run through
tools/perf/framestats.sh) on the same cameras and the track image. Both must give the same window and the
same pieces, one by one and in the same order. The script prints one line per camera and
"ok: all equal" or "FAIL: n differences"; the exit status is 1 on a difference.
The camera lines go to build/stats/walk_cameras.txt and the firmware's answer to the same name with
.result added. The script needs Docker (framestats.sh).
"""
import json
import os
import subprocess
import sys


def main():
    path, image = sys.argv[1:3]
    dumps = json.load(open(path))
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    lines = os.path.join(root, "build", "stats", "walk_cameras.txt")
    with open(lines, "w") as fh:
        for d in dumps:
            v = d["view"]
            fh.write("%d %d %d %.9g\n" % (v[3], v[4], v[5], d["cam_angles"][1]))
    sh = os.path.join(root, "tools", "perf", "framestats.sh")
    # The original's own distances: the window is not scaled.
    env = dict(os.environ, TAG="f1", DEFS="-DFAR_SCALE=1.0f")
    subprocess.run(["sh", sh, "build"], check=True, env=env, stdout=subprocess.DEVNULL)
    subprocess.run(["sh", sh, "walk", image, lines], check=True, env=env)
    ours = open(lines + ".result").read().split("\n")
    # Split the firmware's lines per camera: each camera starts with a W line.
    chunks = []
    for ln in ours:
        if ln.startswith("W "):
            chunks.append([ln])
        elif ln.startswith("P ") and chunks:
            chunks[-1].append(ln)
    bad = 0
    for k, d in enumerate(dumps):
        r = d["rect"]
        row0, col0 = r[1] >> 13, r[0] >> 13
        nrow, ncol = abs(r[3] - r[1]) >> 13, abs(r[2] - r[0]) >> 13
        if row0 + nrow <= 0xFD:
            nrow += 1
        if col0 + ncol <= 0xFD:
            ncol += 1
        want_w = (row0, row0 + nrow, col0, col0 + ncol)
        got_w = tuple(int(x) for x in chunks[k][0].split()[1:])
        want_p = [(p["obj"], p["x"], p["h"], p["z"]) for p in d["reach"] if not p["moving"]]
        got_p = [tuple(int(x) for x in ln.split()[1:]) for ln in chunks[k][1:]]
        ok_w = want_w == got_w and d["col0x4"] == 4 * col0 and d["ncol"] == ncol
        ok_p = want_p == got_p
        print("camera %d (%s): window rows %d..%d columns %d..%d: %s; %d placements in reach, ours %d: %s"
              % (k, d["label"], want_w[0], want_w[1], want_w[2], want_w[3],
                 "equal" if ok_w else "DIFFERENT " + str(got_w),
                 len(want_p), len(got_p), "equal, same order" if ok_p else "DIFFERENT"))
        if not ok_p:
            sw, sg = set(want_p), set(got_p)
            print("   only the original's:", len(sw - sg), "only ours:", len(sg - sw),
                  "first only-original:", sorted(sw - sg)[:3])
        bad += (not ok_w) + (not ok_p)
    print("ok: all equal" if not bad else "FAIL: %d differences" % bad)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
