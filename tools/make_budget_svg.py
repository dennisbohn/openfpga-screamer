"""Draw docs/ressourcen.svg: the FPGA resource budget, measured and estimated.

Usage: python tools/make_budget_svg.py [out.svg]

The numbers live in COMPONENTS below. When synthesis replaces an estimate with a
measured value, set lo = hi to the measured number, mark it measured and re-run.
The table in docs/architektur.md is the text view of the same numbers.
"""
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
    ("frame", "Rahmen & Video (gemessen)", "#2a78d6", "#3987e5"),
    ("cpu", "CPU: VexRiscv, FPU", "#eb6834", "#d95926"),
    ("gfx", "Grafik: Geometrie, Rasterizer, Texturen", "#1baf7a", "#199e70"),
    ("mem", "Speicher-Controller", "#eda100", "#c98500"),
    ("audio", "Audio: OPL2, PCM", "#e87ba4", "#d55181"),
]

# name, group, measured, ALMs (lo, hi), M10K (lo, hi), DSP (lo, hi)
COMPONENTS = [
    ("Analogue-Rahmen", "frame", True, (414, 414), (2, 2), (0, 0)),
    ("Framebuffer + Video", "frame", True, (172, 172), (4, 4), (0, 0)),
    ("VexRiscv + 2× 8 KB Cache", "cpu", False, (1200, 1600), (18, 22), (4, 4)),
    ("FPU (RV32F)", "cpu", False, (1500, 2200), (0, 2), (2, 4)),
    ("Geometrie-Einheit", "gfx", False, (2500, 4000), (4, 8), (12, 18)),
    ("Rasterizer", "gfx", False, (2000, 3000), (3, 6), (4, 8)),
    ("Textur-Cache 16 KB", "gfx", False, (100, 200), (16, 16), (0, 0)),
    ("Schattierungs-/Transparenztabellen", "gfx", False, (0, 0), (16, 24), (0, 0)),
    ("Speicher-Controller", "mem", False, (900, 1400), (2, 4), (0, 0)),
    ("JTOPL (OPL2)", "audio", False, (800, 1200), (3, 5), (0, 1)),
    ("PCM-Mixer", "audio", False, (300, 500), (2, 2), (1, 2)),
]
OPTIONAL = {"m10k": ("Tiefenpuffer (nur Variante B)", "gfx", 100)}
TIMING_LIMIT = 0.80         # logic utilisation above which timing closure gets hard

RES_INDEX = {"alm": 3, "m10k": 4, "dsp": 5}

# layout
WIDTH = 1120
X0, PW = 290, 640           # plot area: x0 .. x0 + pw
GAP = 2                     # surface gap between touching fills
RADIUS = 4


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

    def bar(self, x, y, w, h, cls, title, rounded, style=""):
        """A fill from x to x+w; rounded = 4px rounded data-end on the right."""
        if w <= 0.5:
            return
        st = f' style="{style}"' if style else ""
        t = f"<title>{escape(title)}</title>"
        if not rounded or w < RADIUS * 2:
            self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h}" class="{cls}"{st}>{t}</rect>')
            return
        r = RADIUS
        d = (f"M{x:.1f},{y:.1f} H{x + w - r:.1f} A{r},{r} 0 0 1 {x + w:.1f},{y + r:.1f} "
             f"V{y + h - r:.1f} A{r},{r} 0 0 1 {x + w - r:.1f},{y + h:.1f} H{x:.1f} Z")
        self.add(f'<path d="{d}" class="{cls}"{st}>{t}</path>')


def stacked(svg, y, h, res, values, cap, row_label):
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
    out = sys.argv[1] if len(sys.argv) > 1 else "docs/ressourcen.svg"
    svg = Svg()
    y = 0

    def total(res, which, measured_only=False):
        i = RES_INDEX[res]
        per_group = []
        for key, *_ in GROUPS:
            s = sum(c[i][which] for c in COMPONENTS if c[1] == key and (c[2] or not measured_only))
            per_group.append((key, s))
        return per_group

    # title and legend
    svg.text(40, 40, "Platzbudget im FPGA", "h")
    svg.text(40, 62, "Cyclone V 5CEBA4 · gemessen = aktueller Quartus-Build, sonst geschätzt · Stand 25.10.2026", "s")
    lx = 40
    for key, label, _, _ in GROUPS:
        svg.add(f'<rect x="{lx}" y="84" width="12" height="12" rx="3" class="f-{key}"/>')
        svg.text(lx + 18, 94.5, label, "s")
        lx += 18 + 6.0 * len(label) + 30
    svg.add(f'<rect x="40" y="106" width="22" height="12" rx="3" class="f-cpu"/>')
    svg.add(f'<rect x="64" y="106" width="22" height="12" rx="3" class="f-cpu tint"/>')
    svg.text(94, 116.5, "kräftig: untere Schätzung · hell: Spanne bis zur oberen", "s")
    svg.add('<rect x="430" y="106" width="22" height="12" rx="3" class="opt"/>')
    svg.text(460, 116.5, "optional", "s")

    # section 1: share of the FPGA per resource
    y = 156
    svg.text(40, y, "Anteil an der Kapazität", "t")
    y += 22
    top = y
    for p in (0, 25, 50, 75, 100):
        svg.text(X0 + PW * p / 100, y, f"{p} %", "m", "middle")
    y += 12
    grid_top = y
    rows = []
    for res, name, cap_label in RESOURCES:
        cap = CAPACITY[res]
        y += 8
        svg.text(40, y + 13, name, "t")
        svg.text(40 + 7.6 * len(name) + 10, y + 13, cap_label, "m")
        y += 22
        group_top = y
        for row_label, values in (
            ("heute gebaut", total(res, 0, measured_only=True)),
            ("Plan, untere Schätzung", total(res, 0)),
            ("Plan, obere Schätzung", total(res, 1)),
        ):
            h = 18
            svg.text(X0 - 12, y + 13, row_label, "s", "end")
            end = stacked(svg, y, h, res, values, cap, row_label)
            s = sum(v for _, v in values)
            label = f"{fmt(s)} · {pct(s, cap)} %"
            if res in OPTIONAL and row_label != "heute gebaut":
                oname, ogroup, ov = OPTIONAL[res]
                ow = PW * ov / cap
                svg.add(f'<rect x="{end + GAP:.1f}" y="{y + 0.75:.1f}" width="{ow - GAP:.1f}" height="{h - 1.5}" rx="3" class="opt">'
                        f'<title>{escape(oname)}: +{ov} M10K</title></rect>')
                end += ow
                label += f" · mit Tiefenpuffer {fmt(s + ov)} · {pct(s + ov, cap)} %"
            svg.text(end + 8, y + 13, label, "v")
            y += 28
        if res == "alm":
            lx = X0 + PW * TIMING_LIMIT
            svg.add(f'<line x1="{lx:.1f}" y1="{group_top - 4}" x2="{lx:.1f}" y2="{y - 6}" class="limit"/>')
            svg.text(lx - 6, y + 8, f"ab ~{round(TIMING_LIMIT * 100)} % wird es schwer, den Takt zu halten", "m", "end")
            y += 14
        rows.append(y)
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
    comps = sorted((c for c in COMPONENTS if c[3][1] > 0), key=lambda c: -c[3][1])
    for name, g, measured, (lo, hi), _, _ in comps:
        y += 5
        h = 16
        svg.text(X0 - 12, y + 12, name, "s", "end")
        wlo, whi = PW * lo / scale_max, PW * hi / scale_max
        if measured:
            svg.bar(X0, y, wlo, h, f"f-{g}", f"{name}: {fmt(lo)} ALMs, gemessen", True)
            label = f"{fmt(lo)} · gemessen"
        else:
            svg.bar(X0, y, wlo - GAP, h, f"f-{g}", f"{name}: mindestens {fmt(lo)} ALMs (Schätzung)", False)
            svg.bar(X0 + wlo, y, whi - wlo, h, f"f-{g} tint", f"{name}: bis {fmt(hi)} ALMs (Schätzung)", True)
            label = f"{fmt(lo)}–{fmt(hi)}"
        svg.text(X0 + whi + 8, y + 12, label, "v")
        y += 21
    grid2_bottom = y + 4
    for v in range(0, scale_max + 1, 1000):
        x = X0 + PW * v / scale_max
        svg.parts.insert(0, f'<line x1="{x:.1f}" y1="{grid2_top}" x2="{x:.1f}" y2="{grid2_bottom}" class="{"base" if v == 0 else "grid"}"/>')

    y += 36
    svg.text(40, y, "Schätzungen gelten, bis die Module synthetisiert sind. Textfassung der Zahlen: docs/architektur.md.", "s")
    height = y + 24

    lo_alm = sum(c[3][0] for c in COMPONENTS)
    hi_alm = sum(c[3][1] for c in COMPONENTS)
    lo_m = sum(c[4][0] for c in COMPONENTS)
    hi_m = sum(c[4][1] for c in COMPONENTS)
    lo_d = sum(c[5][0] for c in COMPONENTS)
    hi_d = sum(c[5][1] for c in COMPONENTS)
    aria = (f"Platzbudget im FPGA: Logik geplant {pct(lo_alm, CAPACITY['alm'])} bis {pct(hi_alm, CAPACITY['alm'])} Prozent, "
            f"knapp an der 80-Prozent-Grenze; Block-RAM {pct(lo_m, CAPACITY['m10k'])} bis {pct(hi_m, CAPACITY['m10k'])} Prozent, "
            f"mit Tiefenpuffer {pct(lo_m + 100, CAPACITY['m10k'])} bis {pct(hi_m + 100, CAPACITY['m10k'])}; "
            f"DSP {pct(lo_d, CAPACITY['dsp'])} bis {pct(hi_d, CAPACITY['dsp'])} Prozent. "
            "Größte Posten: Geometrie-Einheit, Rasterizer und FPU.")

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
  .tint {{ fill-opacity: 0.38; }}
  .opt {{ fill: none; stroke: var(--gfx); stroke-width: 1.5; stroke-dasharray: 4 3; }}
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


main()
