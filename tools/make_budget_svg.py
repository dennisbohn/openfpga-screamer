"""Draw docs/ressourcen.svg: the FPGA resource budget, measured and planned.

Usage: python tools/make_budget_svg.py [fit.rpt] [out.svg]

The measured numbers come from the fitter report of the last Quartus build
(core/src/fpga/output_files/ap_core.fit.rpt, table "Fitter Resource Utilization by
Entity"); PLANNED holds the estimates for what is not built yet. It also rewrites the table in
docs/architektur.md, the text view of the same numbers.
"""
import re
import sys
from xml.sax.saxutils import escape

CAPACITY = {"alm": 18480, "m10k": 308, "dsp": 66}
RESOURCES = [
    ("alm", "Logik", "18.480 ALMs"),
    ("m10k", "Block-RAM", "308 M10K"),
    ("dsp", "DSP-Blöcke", "66"),
]

# Categorical slots 1-5 of the dataviz reference palette (validated light and dark).
GROUPS = [
    ("frame", "Rahmen & Video", "#2a78d6", "#3987e5"),
    ("cpu", "CPU: VexRiscv, FPU, Boot-RAM", "#eb6834", "#d95926"),
    ("gfx", "Grafik: Geometrie bis Blitter", "#1baf7a", "#199e70"),
    ("mem", "SDRAM, Bus, Register", "#eda100", "#c98500"),
    ("audio", "Audio", "#e87ba4", "#d55181"),
]

TOP = "|apf_top"
SOC = "|apf_top|core_top:ic|soc:soc"
FB = "|apf_top|core_top:ic|framebuffer:fb"
VO = "|apf_top|core_top:ic|video_out:vo"
CPU = SOC + "|VexPocketIMFC:cpu"
FPU = CPU + "|FpuCore:FpuPlugin_fpu"
SU = SOC + "|setup:su"
CL = SU + "|clipper:cl"
RS = SOC + "|raster:rs"
RT = RS + "|raster_tex:rt"

# name, group, measured as a sum of (sign, hierarchy node or prefix ending in '*')
MEASURED = [
    ("Analogue-Rahmen (Bridge, PLLs)", "frame", [(1, TOP), (-1, SOC), (-1, FB), (-1, VO)]),
    ("Framebuffer ×2 + Video", "frame", [(1, FB), (1, VO)]),
    ("VexRiscv + 2× 8 KB Cache", "cpu", [(1, CPU), (-1, FPU)]),
    ("FPU (RV32F)", "cpu", [(1, FPU)]),
    ("CFU (16.16-Multiplikation)", "cpu", [(1, SOC + "|cfu_fixmul:cfu")]),
    ("Boot-RAM 32 KB + Protokoll", "cpu", [(1, SOC + "|ram_lane:boot_lanes*"), (1, SOC + "|ram_lane:log_lanes*")]),
    ("Geometrie-Einheit", "gfx", [(1, SOC + "|geom:gu")]),
    ("Clipper", "gfx", [(1, CL)]),
    ("Setup-Einheit", "gfx", [(1, SU), (-1, CL)]),
    ("Rasterizer", "gfx", [(1, RS), (-1, RT)]),
    ("Texturen: Pipeline + Cache", "gfx", [(1, RT)]),
    ("Blitter (Hintergrund, HUD)", "gfx", [(1, SOC + "|blit:bl")]),
    ("SDRAM-Controller + Ports", "mem", [(1, SOC + "|sdram:sdc"), (1, SOC + "|sdram_port:sdp")]),
    ("Audio: Mischer 32 Stimmen, I²S", "audio", [(1, SOC + "|audio:au")]),
]
# the SoC's own logic (bus, registers, arbiters): the SoC minus everything measured inside it
SOC_REST = ("Bus, Register, Arbiter", "mem")

# name, group, ALMs (lo, hi), M10K (lo, hi), DSP (lo, hi)
PLANNED = [
]
TIMING_LIMIT = 0.80         # logic utilisation above which timing closure gets hard
STAND = "30.10.2026"
BUILD = "Audio: Mischer"

# layout
WIDTH = 1120
X0, PW = 290, 640           # plot area: x0 .. x0 + pw
GAP = 2                     # surface gap between touching fills
RADIUS = 4


def read_report(path):
    """{hierarchy name: (ALMs needed, M10K, DSP)} from the fitter's entity table."""
    nodes = {}
    inside = False
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("; Fitter Resource Utilization by Entity"):
                inside = True
                continue
            if not inside:
                continue
            if line.startswith(";") and "Compilation Hierarchy Node" not in line:
                f = [s.strip() for s in line.split(";")]
                if len(f) < 16:
                    if nodes:
                        break
                    continue
                alm = float(re.match(r"-?[\d.]+", f[2]).group())     # packing can leave -0.1
                nodes[f[15]] = (alm, int(f[11]), int(f[12]))
            elif nodes and not line.startswith(("+", ";")):
                break
    if TOP not in nodes:
        sys.exit(f"{path}: no entity table")
    return nodes


def measure(nodes, terms):
    total = [0.0, 0, 0]
    for sign, name in terms:
        hits = [v for k, v in nodes.items() if (k.startswith(name[:-1]) and "|" not in k[len(name) - 1:]
                                                 if name.endswith("*") else k == name)]
        if not hits:
            sys.exit(f"not in the report: {name}")
        for v in hits:
            for i in range(3):
                total[i] += sign * v[i]
    return round(total[0]), total[1], total[2]


def components(nodes):
    """[(name, group, measured, (alm lo, hi), (m10k lo, hi), (dsp lo, hi))]"""
    out = []
    inside = [0.0, 0, 0]
    for name, group, terms in MEASURED:
        a, m, d = measure(nodes, terms)
        out.append((name, group, True, (a, a), (m, m), (d, d)))
        if all(t[1].startswith(SOC + "|") for t in terms if t[0] > 0):
            inside = [inside[0] + a, inside[1] + m, inside[2] + d]
    s = nodes[SOC]
    rest = (round(s[0] - inside[0]), s[1] - inside[1], s[2] - inside[2])
    out.append((SOC_REST[0], SOC_REST[1], True, (rest[0],) * 2, (rest[1],) * 2, (rest[2],) * 2))
    built = [sum(c[3 + i][0] for c in out) for i in range(3)]
    top = nodes[TOP]
    assert abs(built[0] - top[0]) <= len(out) and built[1:] == [top[1], top[2]], (built, top)
    out += [(n, g, False, a, m, d) for n, g, a, m, d in PLANNED]
    return out


def fmt(n):
    return f"{n:,}".replace(",", ".")


def pct(value, cap):
    return round(100 * value / cap)


class Svg:
    def __init__(self):
        self.parts = []

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, cls, anchor=None):
        a = f' text-anchor="{anchor}"' if anchor else ""
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" class="{cls}"{a}>{escape(s)}</text>')

    def bar(self, x, y, w, h, cls, title, rounded):
        """A fill from x to x+w; rounded = 4px rounded data-end on the right."""
        if w <= 0.5:
            return
        t = f"<title>{escape(title)}</title>"
        if not rounded or w < RADIUS * 2:
            self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h}" class="{cls}">{t}</rect>')
            return
        r = RADIUS
        d = (f"M{x:.1f},{y:.1f} H{x + w - r:.1f} A{r},{r} 0 0 1 {x + w:.1f},{y + r:.1f} "
             f"V{y + h - r:.1f} A{r},{r} 0 0 1 {x + w - r:.1f},{y + h:.1f} H{x:.1f} Z")
        self.add(f'<path d="{d}" class="{cls}">{t}</path>')


def stacked(svg, y, h, values, cap, row_label):
    """One stacked bar: values = [(group key, built, planned)] in group order; the planned
    part follows its group's built part, light. Returns end x."""
    x = X0
    shown = [(g, v, p) for g, b, pl in values for v, p in ((b, False), (pl, True)) if v > 0]
    for i, (g, v, planned) in enumerate(shown):
        w = PW * v / cap
        last = i == len(shown) - 1
        name = next(label for key, label, _, _ in GROUPS if key == g)
        what = "geplant" if planned else "gebaut"
        svg.bar(x, y, w if last else w - GAP, h, f"f-{g}{' tint' if planned else ''}",
                f"{name}, {what}: {fmt(v)} ({pct(v, cap)} %) · {row_label}", last)
        x += w
    return x


def main():
    report = sys.argv[1] if len(sys.argv) > 1 else "core/src/fpga/output_files/ap_core.fit.rpt"
    out = sys.argv[2] if len(sys.argv) > 2 else "docs/ressourcen.svg"
    comps = components(read_report(report))
    res_index = {"alm": 3, "m10k": 4, "dsp": 5}
    svg = Svg()

    def total(res, which, measured_only=False):
        i = res_index[res]
        return [(key, sum(c[i][which] for c in comps if c[1] == key and c[2]),
                 0 if measured_only else sum(c[i][which] for c in comps if c[1] == key and not c[2]))
                for key, *_ in GROUPS]

    # title and legend
    svg.text(40, 40, "Platzbudget im FPGA", "h")
    svg.text(40, 62, f"Cyclone V 5CEBA4 · gebaut = gemessen im Quartus-Build ({BUILD}) · geplant = geschätzt · "
                     f"Stand {STAND}", "s")
    lx = 40
    for key, label, _, _ in GROUPS:
        svg.add(f'<rect x="{lx}" y="84" width="12" height="12" rx="3" class="f-{key}"/>')
        svg.text(lx + 18, 94.5, label, "s")
        lx += 18 + 6.0 * len(label) + 30
    svg.add('<rect x="40" y="106" width="22" height="12" rx="3" class="f-gfx"/>')
    svg.text(68, 116.5, "gebaut, gemessen", "s")
    svg.add('<rect x="186" y="106" width="22" height="12" rx="3" class="f-gfx tint"/>')
    svg.add('<rect x="210" y="106" width="22" height="12" rx="3" class="f-gfx tint2"/>')
    svg.text(238, 116.5, "geplant, geschätzt: untere Schätzung, heller die Spanne bis zur oberen", "s")

    # section 1: share of the FPGA per resource
    y = 156
    svg.text(40, y, "Anteil an der Kapazität", "t")
    y += 22
    for p in (0, 25, 50, 75, 100):
        svg.text(X0 + PW * p / 100, y, f"{p} %", "m", "middle")
    y += 12
    grid_top = y
    for res, name, cap_label in RESOURCES:
        cap = CAPACITY[res]
        y += 8
        svg.text(40, y + 13, name, "t")
        svg.text(40 + 7.6 * len(name) + 10, y + 13, cap_label, "m")
        y += 22
        group_top = y
        for row_label, values in (
            ("heute gebaut", total(res, 0, measured_only=True)),
            ("mit Geplantem, untere Schätzung", total(res, 0)),
            ("mit Geplantem, obere Schätzung", total(res, 1)),
        ):
            h = 18
            svg.text(X0 - 12, y + 13, row_label, "s", "end")
            end = stacked(svg, y, h, values, cap, row_label)
            s = sum(b + p for _, b, p in values)
            lx = X0 + PW * TIMING_LIMIT
            tx = lx + 6 if res == "alm" and lx - 8 < end + 8 < lx + 6 else end + 8    # clear of the limit line
            svg.text(tx, y + 13, f"{fmt(s)} · {pct(s, cap)} %", "v")
            y += 28
        if res == "alm":
            lx = X0 + PW * TIMING_LIMIT
            svg.add(f'<line x1="{lx:.1f}" y1="{group_top - 4}" x2="{lx:.1f}" y2="{y - 6}" class="limit"/>')
            svg.text(lx - 6, y + 8, f"ab ~{round(TIMING_LIMIT * 100)} % wird es schwer, den Takt zu halten", "m", "end")
            y += 14
    grid_bottom = y
    for p in (0, 25, 50, 75, 100):
        x = X0 + PW * p / 100
        svg.parts.insert(0, f'<line x1="{x:.1f}" y1="{grid_top}" x2="{x:.1f}" y2="{grid_bottom}" class="{"base" if p == 0 else "grid"}"/>')

    # section 2: logic per component
    y += 34
    svg.text(40, y, "Logik je Komponente", "t")
    svg.text(40 + 7.4 * len("Logik je Komponente") + 10, y, "ALMs", "m")
    y += 22
    scale_max = 4000
    for v in range(0, scale_max + 1, 1000):
        svg.text(X0 + PW * v / scale_max, y, fmt(v), "m", "middle")
    y += 12
    grid2_top = y
    for name, g, measured, (lo, hi), _, _ in sorted((c for c in comps if c[3][1] > 0), key=lambda c: -c[3][1]):
        y += 5
        h = 16
        svg.text(X0 - 12, y + 12, name, "s", "end")
        wlo, whi = PW * lo / scale_max, PW * hi / scale_max
        if measured:
            svg.bar(X0, y, wlo, h, f"f-{g}", f"{name}: {fmt(lo)} ALMs, gemessen", True)
            label = fmt(lo)
        else:
            svg.bar(X0, y, wlo - GAP, h, f"f-{g} tint", f"{name}: mindestens {fmt(lo)} ALMs (Schätzung)", False)
            svg.bar(X0 + wlo, y, whi - wlo, h, f"f-{g} tint2", f"{name}: bis {fmt(hi)} ALMs (Schätzung)", True)
            label = f"{fmt(lo)}–{fmt(hi)} · geplant"
        svg.text(X0 + whi + 8, y + 12, label, "v")
        y += 21
    grid2_bottom = y + 4
    for v in range(0, scale_max + 1, 1000):
        x = X0 + PW * v / scale_max
        svg.parts.insert(0, f'<line x1="{x:.1f}" y1="{grid2_top}" x2="{x:.1f}" y2="{grid2_bottom}" class="{"base" if v == 0 else "grid"}"/>')

    y += 36
    svg.text(40, y, "Gemessenes liest tools/make_budget_svg.py aus dem Fitter-Report des letzten Builds. "
                    "Textfassung der Zahlen: docs/architektur.md.", "s")
    height = y + 24

    def span(i, cap):
        lo = sum(c[i][0] for c in comps)
        hi = sum(c[i][1] for c in comps)
        now = sum(c[i][0] for c in comps if c[2])
        return pct(now, cap), pct(lo, cap), pct(hi, cap)

    a, m, d = span(3, CAPACITY["alm"]), span(4, CAPACITY["m10k"]), span(5, CAPACITY["dsp"])
    biggest = [c[0] for c in sorted(comps, key=lambda c: -c[3][1])[:3]]
    aria = (f"Platzbudget im FPGA: Logik heute {a[0]} Prozent, mit Geplantem {a[1]} bis {a[2]} Prozent, "
            f"{'über' if a[2] > 80 else 'unter'} der 80-Prozent-Grenze; Block-RAM heute {m[0]}, geplant {m[1]} bis {m[2]} Prozent; "
            f"DSP heute {d[0]}, geplant {d[1]} bis {d[2]} Prozent. Größte Posten: {', '.join(biggest)}.")

    light = "\n".join(f"    --{k}: {lc};" for k, _, lc, _ in GROUPS)
    dark = "\n".join(f"      --{k}: {dc};" for k, _, _, dc in GROUPS)
    fills = "\n".join(f"  .f-{k} {{ fill: var(--{k}); }}" for k, *_ in GROUPS)
    style = f"""<style>
  svg {{
    --surface: #fcfcfb; --ink: #0b0b0b; --ink2: #52514e; --muted: #898781;
    --grid: #e1e0d9; --base: #c3c2b7;
{light}
  }}
  @media (prefers-color-scheme: dark) {{
    svg {{
      --surface: #1a1a19; --ink: #f0efec; --ink2: #c3c2b7; --muted: #898781;
      --grid: #2c2c2a; --base: #383835;
{dark}
    }}
  }}
  text {{ font-family: system-ui, -apple-system, "Segoe UI", sans-serif; }}
  .h {{ font-size: 20px; font-weight: 700; fill: var(--ink); }}
  .t {{ font-size: 13px; font-weight: 600; fill: var(--ink); }}
  .s {{ font-size: 12px; fill: var(--ink2); }}
  .v {{ font-size: 12px; fill: var(--ink); font-variant-numeric: tabular-nums; }}
  .m {{ font-size: 11px; fill: var(--muted); font-variant-numeric: tabular-nums; }}
{fills}
  .tint {{ fill-opacity: 0.5; }}
  .tint2 {{ fill-opacity: 0.22; }}
  .grid {{ stroke: var(--grid); stroke-width: 1; }}
  .base {{ stroke: var(--base); stroke-width: 1; }}
  .limit {{ stroke: var(--ink2); stroke-width: 1.5; stroke-dasharray: 5 4; }}
</style>"""

    doc = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {height:.0f}" width="{WIDTH}" height="{height:.0f}" '
        f'role="img" aria-label="{escape(aria)}">',
        "<title>Platzbudget im FPGA</title>",
        style,
        f'<rect width="{WIDTH}" height="{height:.0f}" fill="var(--surface)"/>',
    ] + svg.parts + ["</svg>", ""]
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(doc))
    print(f"{out}: {WIDTH} x {height:.0f}")
    for name, group, measured, alm, m10k, dsp in comps:
        r = lambda v: fmt(v[0]) if v[0] == v[1] else f"{fmt(v[0])}–{fmt(v[1])}"
        print(f"  {name:42s} {r(alm):>12s} {r(m10k):>7s} {r(dsp):>5s}{'' if measured else '  geplant'}")
    for res, name, _ in RESOURCES:
        i = res_index[res]
        now = sum(c[i][0] for c in comps if c[2])
        lo = sum(c[i][0] for c in comps)
        hi = sum(c[i][1] for c in comps)
        cap = CAPACITY[res]
        print(f"  {name}: heute {fmt(now)} ({pct(now, cap)} %), mit Geplantem {fmt(lo)}-{fmt(hi)} "
              f"({pct(lo, cap)}-{pct(hi, cap)} %)")
    if out == "docs/ressourcen.svg":
        write_table(comps, "docs/architektur.md")


def write_table(comps, md):
    """The text view of the same numbers in docs/architektur.md: the table under "Belegung" and
    the picture's alternative text, rewritten in place."""
    span = lambda v: fmt(v[0]) if v[0] == v[1] else f"{fmt(v[0])}–{fmt(v[1])}"
    cell = lambda v: "–" if v == (0, 0) else span(v)

    def totals(i, measured_only):
        lo = sum(c[i][0] for c in comps if c[2] or not measured_only)
        hi = sum(c[i][1] for c in comps if c[2] or not measured_only)
        return lo, hi

    def total_cell(i, cap, measured_only):
        lo, hi = totals(i, measured_only)
        p = f"{pct(lo, cap)}" if pct(lo, cap) == pct(hi, cap) else f"{pct(lo, cap)}–{pct(hi, cap)}"
        return f"**{span((lo, hi))} ({p} %)**"

    caps = (CAPACITY["alm"], CAPACITY["m10k"], CAPACITY["dsp"])
    rows = ["| Komponente | ALMs | M10K | DSP |", "|---|---|---|---|"]
    rows += [f"| {c[0]} | {cell(c[3])} | {cell(c[4])} | {cell(c[5])} |" for c in comps if c[2]]
    rows.append("| **gebaut** | " + " | ".join(total_cell(3 + k, caps[k], True) for k in range(3)) + " |")
    rows += [f"| geplant: {c[0]} | {cell(c[3])} | {cell(c[4])} | {cell(c[5])} |" for c in comps if not c[2]]
    rows.append("| **mit Geplantem** | " + " | ".join(total_cell(3 + k, caps[k], False) for k in range(3)) + " |")
    pc = [(pct(totals(i, True)[0], caps[i - 3]), pct(totals(i, False)[0], caps[i - 3]),
           pct(totals(i, False)[1], caps[i - 3])) for i in (3, 4, 5)]
    r = lambda p: f"{p[1]} %" if p[1] == p[2] else f"{p[1]}–{p[2]} %"
    alt = (f"![Platzbudget im FPGA: Logik heute {pc[0][0]} %, mit Geplantem {r(pc[0])}; Block-RAM heute "
           f"{pc[1][0]} %, mit Geplantem {r(pc[1])}; DSP heute {pc[2][0]} %, mit Geplantem {r(pc[2])}](ressourcen.svg)")
    lines = open(md, encoding="utf-8").read().split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith("| Komponente | ALMs |"))
    end = start
    while end < len(lines) and lines[end].startswith("|"):
        end += 1
    lines[start:end] = rows
    lines = [alt if l.startswith("![Platzbudget im FPGA:") else l for l in lines]
    with open(md, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print(f"{md}: table and picture text rewritten")


if __name__ == "__main__":
    main()
