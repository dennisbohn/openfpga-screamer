"""Cost model of a frame, calibrated on the six start frames of Screamer 1 in the simulation.

The model is applied to the frame statistics of tools/perf/framestats.sh (docs/development.md,
"Frame statistics"). This file is a library for tools/perf/study.py. Run alone, it checks the
model: each start frame is predicted from the other five.

Usage: python -I tools/perf/perf_model.py [build dir]            (default: build)
Inputs (all under build/, never committed):
  sim/<set>_t1.log .. t6.log   the simulation's log of the start frame of track n: the CPU phases,
                               the unit counters and the timeline marks
                               (SIM_TRACK=n tools/simcheck.sh <set>_t<n>); <set> defaults to "base"
  stats/out/start1.csv ..      the PC renderer's counters for the same frame
                               (framestats.sh race n label out 4 1)

The model: a frame is CPU phases in sequence, and the geometry unit runs behind the CPU.
  T_gu  = A + C + GU + tail         the geometry unit sets the pace (every start frame of Screamer 1)
  T_cpu = A + C + S + P             the CPU alone
  T_rs  = A + C + RS + tail         the rasterizer alone
  T     = max of the three; the one that wins is the bottleneck.
  A     music + physics + preparation before the cull (measured; the mean of the six, a constant)
  C     cull: c0 + c1 * placements tested + c2 * pieces in view            (fitted)
  GU    geometry unit busy: 32.5 * read misses (4 KB cache) + 32.4 * reflection vertices
        + g1 * faces looked at + g2 * RS     (g1 and g2 are fitted; the last term is the unit's wait
        for the units behind it. The variant "px" uses g2 * pixels instead.)
  RS    rasterizer busy: 0.83 * (pixels + 17.3 * texture misses (16 KB) + 41 * triangles
        + 6.5 * rows + 18,000)               (the cost per event comes from the unit's own counters)
  S     CPU scene loop: s1 * static pieces + s2 * object blocks             (fitted)
  tail  from the last piece to the end: the mean of the six
"""
import csv
import glob
import math
import os
import re
import sys

HZ = 74_250_000
BUDGET = HZ // 50                       # 20 ms
MS = 1000.0 / HZ
# Clocks per geometry-unit read miss: a GEOM_STATS run took 283,879 clocks for 8,738 misses of the
# 4 KB cache (docs/architecture.md).
MISS_GU = 283879 / 8738
# Clocks per reflection vertex: 1,110 reflection vertices cost 36,000 more clocks (docs/architecture.md).
REFL = 36000 / 1110
# Clocks of the CPU after the last piece (radar, race screen): from the timeline of track 2 of Screamer 1.
P_CPU = 53123


# ---------------------------------------------------------------------------------------- small maths
def ols(X, y):
    """Least squares: the coefficients x for which X x is closest to y (normal equations)."""
    n, k = len(X), len(X[0])
    A = [[sum(X[r][i] * X[r][j] for r in range(n)) for j in range(k)] for i in range(k)]
    b = [sum(X[r][i] * y[r] for r in range(n)) for i in range(k)]
    for i in range(k):
        p = max(range(i, k), key=lambda r: abs(A[r][i]))
        A[i], A[p] = A[p], A[i]
        b[i], b[p] = b[p], b[i]
        for r in range(i + 1, k):
            f = A[r][i] / A[i][i]
            for c in range(i, k):
                A[r][c] -= f * A[i][c]
            b[r] -= f * b[i]
    x = [0.0] * k
    for i in range(k - 1, -1, -1):
        x[i] = (b[i] - sum(A[i][j] * x[j] for j in range(i + 1, k))) / A[i][i]
    return x


def mean(v):
    return sum(v) / len(v)


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


# ---------------------------------------------------------------------------------------- the swap rule
def swap_period(T_ms, n=1500):
    """Mean time between two picture swaps (ms) when every frame takes T_ms: the rule of
    core/src/fpga/core/video_out.v (a swap at the start of a blanking line, lines 210..511 and
    0..9 of 512, once per blanking, the CPU starting its next frame at the swap)."""
    L, line = 512, 20.0 / 512
    s, armed = 210.0, 210 + (L - 210) + 10
    first = None
    for k in range(n):
        ln = int(math.ceil(s + T_ms / line))
        while not ((ln % L < 10 or ln % L >= 210) and ln >= armed):
            ln += 1
        if k == 0:
            first = ln
        s = float(ln)
        y = ln % L
        armed = ln + ((10 - y) % L if y != 10 else L)
        if armed <= ln:
            armed += L
    return (s - first) * line / (n - 1)


_period_cache = {}


def fps_of(T_clocks):
    """The frame rate for a frame time in clocks, with the picture swaps of swap_period."""
    T = round(T_clocks * MS * 10) / 10          # 0.1 ms steps
    if T not in _period_cache:
        _period_cache[T] = swap_period(max(T, 5.0))
    return 1000.0 / _period_cache[T]


# ---------------------------------------------------------------------------------------- S1 data
def parse_log(path):
    """The numbers of a simulation log's second track frame: CPU phases, unit counters, timeline marks."""
    txt = open(path, encoding="utf-8", errors="replace").read()
    line = re.search(r"track frame 2: .*", txt).group(0)
    d = {"phys": int(re.search(r"\((\d+) now, (\d+) cycles\)", line).group(2)),
         "phys_now": int(re.search(r"\((\d+) now, (\d+) cycles\)", line).group(1))}
    for k in ["pieces", "faces", "px", "cpu", "geometry", "setup", "raster", "misses", "blit", "backdrop"]:
        mm = re.search(r"(\d+) %s\b|%s (\d+)" % (k, k), line)
        d[k] = int(mm.group(1) or mm.group(2))
    mm = re.search(r"cycles: cull (\d+), pieces (\d+), out (\d+) \(wait (\d+)\), "
                   r"after (\d+), effects (\d+) \+ (\d+)", txt)
    d.update(dict(zip(["cull", "pieces_cpu", "out", "out_wait", "after", "fx_obj", "fx_radar"],
                      map(int, mm.groups()))))
    mm = re.search(r"raster: miss (\d+), edges (\d+), rows (\d+), drain (\d+), starved (\d+)", txt)
    d.update(dict(zip(["r_miss", "r_edges", "r_rows", "r_drain", "r_starved"], map(int, mm.groups()))))
    i = txt.index("timeline (t gu su rs bl wait tag):")
    marks = []
    for ln in txt[i:].splitlines()[1:]:
        if not ln.startswith("    ") or not re.match(r"^[\d |]+$", ln.strip()):
            break
        for mk in ln.split("|"):
            v = tuple(int(x) for x in mk.split())
            if len(v) == 7:
                marks.append(v)
    tag = lambda t: next((m[0] for m in marks if m[6] == t), None)
    d["t_prep"], d["t_first"], d["t_scene"], d["t_hud"] = tag(2), tag(3), tag(5), tag(7)
    d["T"] = d["t_prep"] + d["cpu"]
    return d


def read_csv(path):
    """The rows of a CSV file of framestats as dictionaries; all values are numbers except label and kind."""
    out = []
    for r in csv.DictReader(open(path)):
        out.append({k: (v if k in ("label", "kind") else float(v)) for k, v in r.items()})
    return out


def derived(d):
    """Adds to the frame dictionary d the regressors that every frame has, computed from the PC counters."""
    d["static"] = d["gu_pieces"] - d["obj_blocks"] - d["multi_blocks"]
    return d


# ---------------------------------------------------------------------------------------- model
class Model:
    """The cost model; the formulas are in the header of this file.

    gu_rows are frames that fit the costs of the units (the geometry unit, the rasterizer and the
    tail), which do not depend on the CPU's code. cpu_rows are frames of the firmware that is
    modelled; they fit the CPU parts (the work before the scene, the cull, the loop that hands the
    pieces over, and the rest after it). Without cpu_rows, gu_rows serve for both."""

    def rs_events(self, d):
        """The rasterizer's work in events: pixels plus the weighted misses, triangles, rows and drain."""
        return (d["px_span"] + self.rs_miss * d["tex_miss"] + self.rs_edge * d["walk_tris"]
                + self.rs_row * d["rows"] + self.rs_drain)

    def __init__(self, gu_rows, cpu_rows=None):
        cpu_rows = cpu_rows or gu_rows
        rows = gu_rows
        # The rasterizer: the cost per event comes from its own counters (waiting for lines, per
        # triangle, per row).
        self.rs_miss = mean([d["r_miss"] / d["tex_miss"] for d in rows])
        self.rs_edge = mean([d["r_edges"] / d["walk_tris"] for d in rows])
        self.rs_row = mean([d["r_rows"] / d["rows"] for d in rows])
        self.rs_drain = mean([d["r_drain"] for d in rows])
        self.rs_over = mean([d["raster"] / self.rs_events(d) for d in rows])
        rest = lambda d: d["geometry"] - MISS_GU * d["gu_miss"] - REFL * d["refl_verts"]
        self.g2 = ols([[d["faces_row"], self.rs(d)] for d in rows], [rest(d) for d in rows])
        self.tail = mean([d["T"] - d["t_first"] - d["geometry"] for d in rows])
        # The CPU: a frame on the grid has one physics step in 5 of the 6 simulated frames (none in one).
        with_step = [d for d in cpu_rows if d["phys_now"]]
        self.A = mean([d["t_prep"] for d in with_step])
        # The cull: so many clocks for a placement tested (c1), a piece in view (c2) and a cell passed
        # (c3), the rest a constant (c0). c1 to c3 are fixed values from a fit; the cull of the game's
        # window walks cells.
        self.c1, self.c2, self.c3 = 150.0, 110.0, 8.0
        self.c0 = mean([d["cull"] - self.c1 * d["tested"] - self.c2 * d["visible"]
                        - self.c3 * d.get("cells_rows", 0) for d in cpu_rows])
        self.s = ols([[d["static"], d["obj_blocks"]] for d in cpu_rows],
                     [d["pieces_cpu"] + d["out"] - d["out_wait"] for d in cpu_rows])
        hud = [d["t_hud"] - d["t_scene"] for d in cpu_rows if d.get("t_hud") and d.get("t_scene")]
        self.p_cpu = mean(hud) if hud else P_CPU

    def cull(self, d):
        """The predicted clocks of the cull."""
        return self.c0 + self.c1 * d["tested"] + self.c2 * d["visible"] + self.c3 * d.get("cells_rows", 0)

    def gu_fixed(self, d):
        """The geometry unit's time for read misses and reflection vertices, which needs no fit."""
        return MISS_GU * d["gu_miss"] + REFL * d["refl_verts"]

    def gu(self, d):
        """The predicted clocks the geometry unit is busy."""
        return self.gu_fixed(d) + self.g2[0] * d["faces_row"] + self.g2[1] * self.rs(d)

    def rs(self, d):
        """The predicted clocks the rasterizer is busy."""
        return self.rs_over * self.rs_events(d)

    def scene_cpu(self, d):
        """The predicted clocks of the CPU's scene loop."""
        return self.s[0] * d["static"] + self.s[1] * d["obj_blocks"]

    def frame(self, d, a_extra=0.0):
        """The predicted clocks of a frame and of its parts, and which unit is the bottleneck.

        a_extra is added to A, the work before the cull."""
        A, C, G, S = self.A + a_extra, self.cull(d), self.gu(d), self.scene_cpu(d)
        t_gu = A + C + G + self.tail
        t_cpu = A + C + S + self.p_cpu
        t_rs = A + C + self.rs(d) + self.tail
        T = max(t_gu, t_cpu, t_rs)
        which = "GU" if T == t_gu else "CPU" if T == t_cpu else "RS"
        return {"T": T, "bound": which, "A": A, "C": C, "GU": G, "S": S, "RS": self.rs(d),
                "t_gu": t_gu, "t_cpu": t_cpu, "t_rs": t_rs}


# ---------------------------------------------------------------------------------------- the start frames
def calibration(build="build", prefix="base", start="start"):
    """The six start frames: the simulation's log (<prefix>_t<n>.log) with the PC counters of the same frame.

    The PC counters are the row of <start><n>.csv (the race's steps 1..4) whose number of faces is
    closest to the log's. A frame has one to three steps on the grid."""
    rows = []
    for n in range(1, 7):
        d = parse_log(f"{build}/sim/{prefix}_t{n}.log")
        cands = read_csv(f"{build}/stats/out/{start}{n}.csv")
        c = min(cands, key=lambda r: abs(r["ts_faces"] - d["faces"]))
        d.update(c)
        d["n"] = n
        rows.append(derived(d))
    return rows


def main():
    """Checks the model against the simulation and prints a table.

    Each start frame of the modelled firmware (log prefix p2: the constants of this tree) is
    predicted from the other five of that set and the six frames with log prefix base."""
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    base = calibration(build, "base", "start")
    new = calibration(build, "p2", "start_p2")
    print("| Track | T measured | T predicted | error | cull | GU | RS | CPU path | bound |")
    print("|---|---|---|---|---|---|---|---|---|")
    errs = []
    for k in range(6):
        others = [d for i, d in enumerate(new) if i != k]
        m = Model(base + others, others)
        d = new[k]
        f = m.frame(d)
        errs.append(f["T"] / d["T"] - 1)
        print(f"| {d['n']} | {d['T']:,} ({d['T'] * MS:.1f} ms) | {f['T']:,.0f} ({f['T'] * MS:.1f} ms) | "
              f"{100 * errs[-1]:+.1f} % | "
              f"{100 * (m.cull(d) / d['cull'] - 1):+.1f} % | {100 * (m.gu(d) / d['geometry'] - 1):+.1f} % | "
              f"{100 * (m.rs(d) / d['raster'] - 1):+.1f} % | {m.frame(d)['t_cpu'] / d['T']:.2f} | "
              f"{f['bound']} |")
    print(f"hold-out error of T: mean {100 * mean(errs):+.1f} %, worst {100 * max(errs, key=abs):+.1f} %, "
          f"rms {100 * math.sqrt(mean([e * e for e in errs])):.1f} %")


if __name__ == "__main__":
    main()
