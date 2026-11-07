"""The tables of the draw-distance study (docs/development.md, "Draw distance and level of detail").

Usage: python -I tools/perf/study.py [build dir]            (default: build)
Reads build/stats/study/r_<FARSCALE>_<LODSCALE>_t<n>.csv (tools/perf/study_sweeps.sh: a race of the robot on every
track, 600 frames each) and old_t<n>.csv (the same race with the firmware of commit 308fd2b), and the frame-time model
(tools/perf/perf_model.py) fitted on the six start frames of the simulation of the old firmware (build/sim/base_t*.log)
and the six of this tree's firmware (build/sim/p2_t*.log, with start_p2<n>.csv). Prints markdown tables and writes
build/stats/study/report.md.
"""
import csv
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_model as pm


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


def load(path):
    rows = pm.read_csv(path)
    for r in rows:
        pm.derived(r)
    return rows


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    base = pm.calibration(build, "base", "start")
    new = pm.calibration(build, "p2", "start_p2")
    m_old = pm.Model(base, base)
    m = pm.Model(base + new, new)
    ms = pm.MS
    out = []
    P = lambda s="": out.append(s)
    st = f"{build}/stats/study"

    def stats(model, tracks_rows):
        """tracks_rows: {track: [frames]} -> per track and overall: mean ms, p95, worst, share within 20 ms, bound shares"""
        res = {}
        allT = []
        for n, rows in tracks_rows.items():
            fr = [model.frame(r) for r in rows]
            T = [f["T"] * ms for f in fr]
            res[n] = (sum(T) / len(T), pct(T, 0.95), max(T), 100.0 * sum(1 for t in T if t <= 20.0) / len(T),
                      {b: sum(1 for f in fr if f["bound"] == b) for b in ("GU", "CPU", "RS")})
            allT += T
        res["all"] = (sum(allT) / len(allT), pct(allT, 0.95), max(allT), 100.0 * sum(1 for t in allT if t <= 20.0) / len(allT))
        return res

    def means(tracks_rows):
        keys = ("ts_pieces", "faces_row", "verts", "px_span", "tex_miss", "tested", "cells_rows")
        tot = {k: 0.0 for k in keys}
        n = 0
        for rows in tracks_rows.values():
            for r in rows:
                for k in keys:
                    tot[k] += r.get(k, 0.0)
                n += 1
        return {k: v / n for k, v in tot.items()}

    sets = {}
    for fs in ("1.0", "1.25", "1.5"):
        for ls in ("0", "1.0", "1.25", "1.5"):
            sets[(fs, ls)] = {n: load(f"{st}/r_{fs}_{ls}_t{n}.csv") for n in range(1, 7)}
    old = {n: load(f"{st}/old_t{n}.csv") for n in range(1, 7)}

    P("## Calibration (start frames of the simulation; hold-out in tools/perf/perf_model.py)")
    P()
    P(f"A (before the cull, with a physics step) {m.A:,.0f} clocks, cull = {m.c0:,.0f} + {m.c1:.0f} x tested + {m.c2:.0f} x in view + "
      f"{m.c3:.0f} x cells passed, scene loop S = {m.s[0]:.0f} x static pieces + {m.s[1]:.0f} x object blocks, after the last piece "
      f"{m.p_cpu:,.0f}, tail {m.tail:,.0f}; GU = {pm.MISS_GU:.1f} x misses + {pm.REFL:.1f} x reflection vertices + {m.g2[0]:.1f} x faces "
      f"looked at + {m.g2[1]:.2f} x RS. The old firmware's: A {m_old.A:,.0f}, cull {m_old.c0:,.0f} + 150 x tested + 110 x in view.")
    P()

    P("## The matrix: means over the six races (3,600 frames), the modelled frame time")
    P()
    P("Rows: the far plane and reach x FAR_SCALE and the level of detail x LOD_SCALE (off = every block at level 0). "
      "Pieces handed on, faces and vertices looked at by the geometry unit, pixels and texture-cache misses of the rasterizer, "
      "placements tested; model: mean ms of a frame, the worst frame of any track, share of all frames within 20 ms.")
    P()
    P("| FAR_SCALE | LOD_SCALE | pieces | faces looked at | vertices | pixels | texture misses | placements tested | mean ms | worst ms | within 20 ms |")
    P("|---|---|---|---|---|---|---|---|---|---|---|")
    mo, so = means(old), stats(m_old, old)
    P(f"| the old firmware (308fd2b) | none | {mo['ts_pieces']:.0f} | {mo['faces_row']:.0f} | {mo['verts']:.0f} | {mo['px_span']:.0f} | {mo['tex_miss']:.0f} | "
      f"{mo['tested']:.0f} | {so['all'][0]:.1f} | {so['all'][2]:.1f} | {so['all'][3]:.0f} % |")
    for (fs, ls), rows in sets.items():
        mm, ss = means(rows), stats(m, rows)
        P(f"| {fs} | {'off' if ls == '0' else ls} | {mm['ts_pieces']:.0f} | {mm['faces_row']:.0f} | {mm['verts']:.0f} | {mm['px_span']:.0f} | {mm['tex_miss']:.0f} | "
          f"{mm['tested']:.0f} | {ss['all'][0]:.1f} | {ss['all'][2]:.1f} | {ss['all'][3]:.0f} % |")
    P()

    def per_track(title, model, rows_by_track, bounds=False):
        P(f"### {title}")
        P()
        P("| Track | mean ms | p95 ms | worst ms | within 20 ms | bound: GU / CPU / RS frames |")
        P("|---|---|---|---|---|---|")
        s = stats(model, rows_by_track)
        for n in range(1, 7):
            v = s[n]
            P(f"| {n} | {v[0]:.1f} | {v[1]:.1f} | {v[2]:.1f} | {v[3]:.0f} % | {v[4]['GU']} / {v[4]['CPU']} / {v[4]['RS']} |")
        P()

    P("## Per track")
    P()
    per_track("The old firmware (308fd2b: reach 52,000, far plane 43,808, no level of detail, no window)", m_old, old)
    per_track("FAR_SCALE 1.0, LOD_SCALE off (the original's distances, the game's cell window)", m, sets[("1.0", "0")])
    per_track("FAR_SCALE 1.0, LOD_SCALE 1.0 (the original's rule)", m, sets[("1.0", "1.0")])
    per_track("FAR_SCALE 1.5, LOD_SCALE off", m, sets[("1.5", "0")])
    per_track("FAR_SCALE 1.5, LOD_SCALE 1.5 (the defaults)", m, sets[("1.5", "1.5")])
    txt = "\n".join(out)
    print(txt)
    open(f"{st}/report.md", "w", encoding="utf-8").write(txt + "\n")


if __name__ == "__main__":
    main()
