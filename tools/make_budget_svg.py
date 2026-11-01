"""Draw docs/resources.svg: the FPGA resource budget of the last build.

Usage: python tools/make_budget_svg.py [fit.rpt] [out.svg]

The measured numbers come from the fitter report of the last Quartus build
(core/src/fpga/output_files/ap_core.fit.rpt, table "Fitter Resource Utilization by
Entity").
"""
import re
import sys
from xml.sax.saxutils import escape

CAPACITY = {"alm": 18480, "m10k": 308, "dsp": 66}
RESOURCES = [
    ("alm", "Logic", "18,480 ALMs"),
    ("m10k", "Block RAM", "308 M10K"),
    ("dsp", "DSP blocks", "66"),
]

# Categorical slots 1-5 of the dataviz reference palette (validated light and dark).
GROUPS = [
    ("frame", "Frame & video", "#2a78d6", "#3987e5"),
    ("cpu", "CPU: VexRiscv, FPU, boot RAM", "#eb6834", "#d95926"),
    ("gfx", "Graphics: geometry to blitter", "#1baf7a", "#199e70"),
    ("mem", "SDRAM, bus, registers", "#eda100", "#c98500"),
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
    ("Analogue frame (bridge, PLLs)", "frame", [(1, TOP), (-1, SOC), (-1, FB), (-1, VO)]),
    ("Framebuffer ×2 + video", "frame", [(1, FB), (1, VO)]),
    ("VexRiscv + 2× 8 KB cache", "cpu", [(1, CPU), (-1, FPU)]),
    ("FPU (RV32F)", "cpu", [(1, FPU)]),
    ("CFU (16.16 multiply)", "cpu", [(1, SOC + "|cfu_fixmul:cfu")]),
    ("Boot RAM 32 KB + log", "cpu", [(1, SOC + "|ram_lane:boot_lanes*"), (1, SOC + "|ram_lane:log_lanes*")]),
    ("Geometry unit", "gfx", [(1, SOC + "|geom:gu")]),
    ("Clipper", "gfx", [(1, CL)]),
    ("Setup unit", "gfx", [(1, SU), (-1, CL)]),
    ("Rasterizer", "gfx", [(1, RS), (-1, RT)]),
    ("Textures: pipeline + cache", "gfx", [(1, RT)]),
    ("Blitter (backdrop, HUD)", "gfx", [(1, SOC + "|blit:bl")]),
    ("SDRAM controller + ports", "mem", [(1, SOC + "|sdram:sdc"), (1, SOC + "|sdram_port:sdp")]),
    ("Audio: 32-voice mixer + music, I²S", "audio", [(1, SOC + "|audio:au")]),
    ("FM music: OPL2 chip + write queue", "audio", [(1, SOC + "|opl:fm_unit")]),
]
# the SoC's own logic (bus, registers, arbiters): the SoC minus everything measured inside it
SOC_REST = ("Bus, registers, arbiters", "mem")

TIMING_LIMIT = 0.80         # logic utilisation above which timing closure gets hard
STAMP = "2026-11-01"
BUILD = "FM music: OPL2 with its write queue"

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
    """[(name, group, ALMs, M10K, DSP)]"""
    out = []
    inside = [0.0, 0, 0]
    for name, group, terms in MEASURED:
        a, m, d = measure(nodes, terms)
        out.append((name, group, a, m, d))
        if all(t[1].startswith(SOC + "|") for t in terms if t[0] > 0):
            inside = [inside[0] + a, inside[1] + m, inside[2] + d]
    s = nodes[SOC]
    out.append((SOC_REST[0], SOC_REST[1], round(s[0] - inside[0]), s[1] - inside[1], s[2] - inside[2]))
    built = [sum(c[2 + i] for c in out) for i in range(3)]
    top = nodes[TOP]
    assert abs(built[0] - top[0]) <= len(out) and built[1:] == [top[1], top[2]], (built, top)
    return out


def fmt(n):
    return f"{n:,}"


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
    """One stacked bar: values = [(group key, value)] in group order. Returns end x."""
    x = X0
    shown = [(g, v) for g, v in values if v > 0]
    for i, (g, v) in enumerate(shown):
        w = PW * v / cap
        last = i == len(shown) - 1
        name = next(label for key, label, _, _ in GROUPS if key == g)
        svg.bar(x, y, w if last else w - GAP, h, f"f-{g}",
                f"{name}: {fmt(v)} ({pct(v, cap)} %) · {row_label}", last)
        x += w
    return x


def main():
    report = sys.argv[1] if len(sys.argv) > 1 else "core/src/fpga/output_files/ap_core.fit.rpt"
    out = sys.argv[2] if len(sys.argv) > 2 else "docs/resources.svg"
    comps = components(read_report(report))
    res_index = {"alm": 2, "m10k": 3, "dsp": 4}
    svg = Svg()

    def total(res):
        i = res_index[res]
        return [(key, sum(c[i] for c in comps if c[1] == key)) for key, *_ in GROUPS]

    # title and legend
    svg.text(40, 40, "FPGA resource budget", "h")
    svg.text(40, 62, f"Cyclone V 5CEBA4 · measured in the Quartus build ({BUILD}) · {STAMP}", "s")
    lx = 40
    for key, label, _, _ in GROUPS:
        svg.add(f'<rect x="{lx}" y="84" width="12" height="12" rx="3" class="f-{key}"/>')
        svg.text(lx + 18, 94.5, label, "s")
        lx += 18 + 6.0 * len(label) + 30

    # section 1: share of the FPGA per resource
    y = 134
    svg.text(40, y, "Share of capacity", "t")
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
        values = total(res)
        h = 18
        svg.text(X0 - 12, y + 13, "built", "s", "end")
        end = stacked(svg, y, h, values, cap, "built")
        s = sum(v for _, v in values)
        lx = X0 + PW * TIMING_LIMIT
        tx = lx + 6 if res == "alm" and lx - 8 < end + 8 < lx + 6 else end + 8    # clear of the limit line
        svg.text(tx, y + 13, f"{fmt(s)} · {pct(s, cap)} %", "v")
        y += 28
        if res == "alm":
            lx = X0 + PW * TIMING_LIMIT
            svg.add(f'<line x1="{lx:.1f}" y1="{group_top - 4}" x2="{lx:.1f}" y2="{y - 6}" class="limit"/>')
            svg.text(lx - 6, y + 8, f"above ~{round(TIMING_LIMIT * 100)} % timing closure gets hard", "m", "end")
            y += 14
    grid_bottom = y
    for p in (0, 25, 50, 75, 100):
        x = X0 + PW * p / 100
        svg.parts.insert(0, f'<line x1="{x:.1f}" y1="{grid_top}" x2="{x:.1f}" y2="{grid_bottom}" class="{"base" if p == 0 else "grid"}"/>')

    # section 2: logic per component
    y += 34
    svg.text(40, y, "Logic per component", "t")
    svg.text(40 + 7.4 * len("Logic per component") + 10, y, "ALMs", "m")
    y += 22
    scale_max = 4000
    for v in range(0, scale_max + 1, 1000):
        svg.text(X0 + PW * v / scale_max, y, fmt(v), "m", "middle")
    y += 12
    grid2_top = y
    for name, g, alm, _, _ in sorted((c for c in comps if c[2] > 0), key=lambda c: -c[2]):
        y += 5
        h = 16
        svg.text(X0 - 12, y + 12, name, "s", "end")
        svg.bar(X0, y, PW * alm / scale_max, h, f"f-{g}", f"{name}: {fmt(alm)} ALMs", True)
        svg.text(X0 + PW * alm / scale_max + 8, y + 12, fmt(alm), "v")
        y += 21
    grid2_bottom = y + 4
    for v in range(0, scale_max + 1, 1000):
        x = X0 + PW * v / scale_max
        svg.parts.insert(0, f'<line x1="{x:.1f}" y1="{grid2_top}" x2="{x:.1f}" y2="{grid2_bottom}" class="{"base" if v == 0 else "grid"}"/>')

    y += 36
    svg.text(40, y, "Numbers read by tools/make_budget_svg.py from the fitter report of the last build.", "s")
    height = y + 24

    def share(i, cap):
        return pct(sum(c[i] for c in comps), cap)

    a, m, d = share(2, CAPACITY["alm"]), share(3, CAPACITY["m10k"]), share(4, CAPACITY["dsp"])
    biggest = [c[0] for c in sorted(comps, key=lambda c: -c[2])[:3]]
    aria = (f"FPGA resource budget: logic {a} percent, {'above' if a > 80 else 'below'} the 80 percent limit; "
            f"block RAM {m} percent; DSP {d} percent. Largest items: {', '.join(biggest)}.")

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
  .grid {{ stroke: var(--grid); stroke-width: 1; }}
  .base {{ stroke: var(--base); stroke-width: 1; }}
  .limit {{ stroke: var(--ink2); stroke-width: 1.5; stroke-dasharray: 5 4; }}
</style>"""

    doc = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {height:.0f}" width="{WIDTH}" height="{height:.0f}" '
        f'role="img" aria-label="{escape(aria)}">',
        "<title>FPGA resource budget</title>",
        style,
        f'<rect width="{WIDTH}" height="{height:.0f}" fill="var(--surface)"/>',
    ] + svg.parts + ["</svg>", ""]
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(doc))
    print(f"{out}: {WIDTH} x {height:.0f}")
    for name, group, alm, m10k, dsp in comps:
        print(f"  {name:42s} {fmt(alm):>8s} {m10k:>5d} {dsp:>4d}")
    for res, name, _ in RESOURCES:
        now = sum(c[res_index[res]] for c in comps)
        print(f"  {name}: {fmt(now)} ({pct(now, CAPACITY[res])} %)")


if __name__ == "__main__":
    main()
