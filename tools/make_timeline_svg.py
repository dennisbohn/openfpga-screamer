"""Draw docs/bildablauf.svg: one frame of the race over time, from the firmware's timeline.

Usage: python tools/make_timeline_svg.py [log] [out.svg]
  log: a simulation log (build/sim/check.log, the default) or the Pocket's debug.txt; the last
  "timeline" block in it is drawn (main.c tl_log: the last whole frame before the logged one).

Each mark holds the cycles since the frame began and the counters as they stood (the block's
first line names them): the busy clocks of the geometry unit (gu), the setup unit (su, if
there is one), the rasterizer (rs) and the blitter (bl) and the CPU's cycles writing to them
(wait). The counters are cleared during the frame (main.c): all but the blitter's just before
the mark "prep" (before it they still hold the last frame's values), the blitter's before the
backdrop (before "prep") and just before the mark "clear".
"""
import re
import sys
from html import escape

CPU_HZ = 74_250_000
BUDGET = CPU_HZ // 50                   # cycles per picture at 50 fps
STAND = "30.10.2026"

T_START, T_PHYS, T_PREP, T_CULL, T_PIECES, T_SCENE, T_CLEAR, T_HUD, T_WAIT, T_END = range(10)

# the CPU's phases: the mark that ends a stretch names it (categorical slots of the dataviz
# reference palette, light and dark)
PHASES = [
    ("phys", "Physik (Fahrzeug, Gegner)", "#2a78d6", "#3987e5", (T_PHYS,)),
    ("prep", "Vorbereitung (Kamera, Objekte, Hintergrund)", "#eda100", "#c98500", (T_PREP,)),
    ("cull", "Auswahl (Sichtprüfung, Sortierung)", "#eb6834", "#d95926", (T_CULL,)),
    ("pieces", "Bauteile (Matrizen, Übergabe an die Hardware)", "#1baf7a", "#199e70", (T_PIECES, T_SCENE)),
    ("hud", "Rennanzeige und Radar", "#e87ba4", "#d55181", (T_CLEAR, T_HUD)),
    ("wait", "Warten auf die Hardware", "#898781", "#898781", (T_WAIT,)),
    ("rest", "Zähler, Protokoll", "#c3c2b7", "#52514e", (T_END,)),
]
UNIT_NAMES = {"gu": "Geometrie-Einheit", "su": "Setup-Einheit", "rs": "Rasterizer", "bl": "Blitter"}
UNIT_FILL = ("#6250d6", "#9085e9")      # slot 7 (violet), one hue for the units' busy share

WIDTH = 1120
X0, PW = 220, 840
AXIS_END = 21 * CPU_HZ // 1000          # 21 ms: the 20 ms line stays inside


def read_timeline(path):
    """(frame number logged, field names, [marks as dicts]) of the last timeline block."""
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    start = frame = fields = None
    for i, ln in enumerate(lines):
        m = re.match(r"\s*timeline \(([a-z ]+)\):", ln)
        if m:
            start, fields = i, m.group(1).split()
            for j in range(i, -1, -1):
                m = re.search(r"track frame (\d+):", lines[j])
                if m:
                    frame = int(m.group(1))
                    break
    if start is None:
        sys.exit(f"{path}: no timeline")
    nums = []
    for ln in lines[start + 1:]:
        if not ln.startswith("    ") or not re.match(r"^[\d |]+$", ln.strip()):
            break
        for mark in ln.split("|"):
            nums.append(dict(zip(fields, (int(v) for v in mark.split()))))
    return frame, fields, nums


def deltas(marks, counters):
    """[(t0, t1, tag, {counter: busy clocks})]: each stretch between two marks."""
    out = []
    for prev, cur in zip(marks, marks[1:]):
        tag = cur["tag"]
        d = {}
        for name in counters:
            if name == "bl":
                d[name] = 0 if tag in (T_PHYS, T_CLEAR) else cur[name] if tag == T_PREP else cur[name] - prev[name]
            else:
                d[name] = 0 if tag in (T_PHYS, T_PREP) else cur[name] - prev[name]
            d[name] = max(0, d[name])
        out.append((prev["t"], cur["t"], tag, d))
    return out


def fmt(n):
    return f"{n:,}".replace(",", ".")


def ms(c):
    return f"{c * 1000 / CPU_HZ:.1f}".replace(".", ",")


def px(c):
    return X0 + PW * c / AXIS_END


def phase_of(tag):
    for p in PHASES:
        if tag in p[4]:
            return p
    return None


def main():
    log = sys.argv[1] if len(sys.argv) > 1 else "build/sim/check.log"
    out = sys.argv[2] if len(sys.argv) > 2 else "docs/bildablauf.svg"
    frame, fields, marks = read_timeline(log)
    UNITS = [(k, UNIT_NAMES[k]) for k in fields if k in UNIT_NAMES]
    st = deltas(marks, [k for k in fields if k not in ("t", "tag")])
    end = marks[-1]["t"]
    parts = []

    def text(x, y, s, cls, anchor=None):
        a = f' text-anchor="{anchor}"' if anchor else ""
        parts.append(f'<text x="{x:.1f}" y="{y:.1f}" class="{cls}"{a}>{escape(s)}</text>')

    def rect(x, y, w, h, cls, title):
        if w > 0.3 and h > 0.3:
            parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" class="{cls}">'
                         f'<title>{escape(title)}</title></rect>')

    seen = f"Rennbild {frame - 1}" if frame else "ein Rennbild"
    text(40, 36, "Ablauf eines Bildes", "h")
    text(40, 60, f"Simulation, {seen}, Takte der CPU bei 74,25 MHz (20 ms = {fmt(BUDGET)} Takte "
                 f"für 50 Bilder pro Sekunde) · Stand {STAND}", "s")

    # legend: the CPU's phases, then the units' busy share
    lx, ly = 40, 88
    for key, name, *_ in PHASES:
        w = 18 + 7.0 * len(name) + 22
        if lx + w > WIDTH - 40:
            lx, ly = 40, ly + 22
        rect(lx, ly - 10, 12, 12, f"f-{key}", name)
        text(lx + 18, ly, name, "s")
        lx += w
    lx, ly = 40, ly + 22
    rect(lx, ly - 10, 12, 12, "f-unit", "beschäftigt")
    text(lx + 18, ly, "Hardware beschäftigt: die Höhe ist ihr Anteil am Abschnitt "
                      "(eine Einheit zählt ihr Warten auf die nächste mit)", "s")

    # axis: ms above the lanes
    top = ly + 40
    lane_h, gap = 34, 18
    lanes = 1 + len(UNITS)
    bottom = top + 20 + lanes * (lane_h + gap)
    for m in range(0, 22, 2):
        c = m * CPU_HZ // 1000
        x = px(c)
        parts.append(f'<line x1="{x:.1f}" y1="{top + 12}" x2="{x:.1f}" y2="{bottom - gap + 4}" class="grid"/>')
        text(x, top + 4, f"{m} ms", "m", "middle")

    # lanes
    y = top + 20
    text(X0 - 12, y + lane_h / 2 + 4, "CPU", "t", "end")
    runs = {}                           # each phase's stretch (the phases follow each other)
    for t0, t1, tag, d in st:
        p = phase_of(tag)
        if p:
            r = runs.setdefault(p[0], [t0, t1, 0, p[1]])
            r[1], r[2] = t1, r[2] + d["wait"]
    for key, (t0, t1, wait, name) in runs.items():
        title = f"{name}: {fmt(t1 - t0)} Takte ({ms(t1 - t0)} ms)"
        if key == "pieces":
            title += f", davon Schreiben an die Hardware {fmt(wait)}"
        rect(px(t0), y, px(t1) - px(t0), lane_h, f"f-{key}", title)
    if end < BUDGET:
        rect(px(end), y, px(BUDGET) - px(end), lane_h, "idle",
             f"wartet auf den Bildwechsel: {fmt(BUDGET - end)} Takte ({ms(BUDGET - end)} ms)")
        text((px(end) + px(BUDGET)) / 2, y + lane_h / 2 + 4, "frei", "m", "middle")
    # phase names over the CPU lane where they fit
    short = {"phys": "Physik", "prep": "Vorb.", "cull": "Auswahl", "pieces": "Bauteile",
             "hud": "Anzeige", "wait": "", "rest": ""}
    for key, (t0, t1, *_) in runs.items():
        w = px(t1) - px(t0)
        label = short[key]
        if label and w > 7.2 * len(label) + 8:
            text((px(t0) + px(t1)) / 2, y + lane_h / 2 + 4, label, "in", "middle")
    y += lane_h + gap
    for k, (key, name) in enumerate(UNITS):
        text(X0 - 12, y + lane_h / 2 + 4, name, "t", "end")
        rect(X0, y, px(end) - X0, lane_h, "lane", name)
        for t0, t1, tag, d in st:
            b = d[key]
            if t1 > t0 and b:
                f = min(1.0, b / (t1 - t0))
                rect(px(t0), y + lane_h * (1 - f), px(t1) - px(t0), lane_h * f, "f-unit",
                     f"{name}: {fmt(b)} von {fmt(t1 - t0)} Takten beschäftigt ({round(100 * f)} %)")
        y += lane_h + gap

    # the 50 fps line
    xb = px(BUDGET)
    parts.append(f'<line x1="{xb:.1f}" y1="{top + 12}" x2="{xb:.1f}" y2="{y - gap + 8}" class="limit"/>')
    text(xb, y - gap + 22, "Bildwechsel bei 50 fps", "m", "middle")

    # the numbers
    y += 30
    text(40, y, "Die CPU in diesem Bild", "t")
    text(560, y, "Die Hardware in diesem Bild", "t")
    y += 8
    sums = {}
    for t0, t1, tag, *_ in st:
        p = phase_of(tag)
        if p:
            sums[p[0]] = sums.get(p[0], 0) + t1 - t0
    rows = [(name, sums.get(key, 0)) for key, name, *_ in PHASES]
    if end < BUDGET:
        rows.append(("frei bis zum Bildwechsel", BUDGET - end))
    yy = y
    for name, c in rows:
        yy += 20
        text(40, yy, name, "s")
        text(400, yy, fmt(c), "v", "end")
        text(470, yy, f"{ms(c)} ms", "v", "end")
        text(520, yy, f"{round(100 * c / BUDGET)} %", "v", "end")
    hw = [(name, sum(s[3][key] for s in st)) for key, name in UNITS]
    wait = sum(s[3]["wait"] for s in st)
    hw.append(("CPU: Schreiben an die Hardware (mit Warten)", wait))
    yy2 = y
    for name, c in hw:
        yy2 += 20
        text(560, yy2, name, "s")
        text(940, yy2, fmt(c), "v", "end")
        text(1010, yy2, f"{ms(c)} ms", "v", "end")
        text(1060, yy2, f"{round(100 * c / BUDGET)} %", "v", "end")
    y = max(yy, yy2) + 34
    text(40, y, "Prozent: Anteil an den 20 ms eines Bildes. Gemessen mit Zeitmarken der Firmware "
                "(main.c, tl_mark) und den Taktzählern der Einheiten; gezeichnet von "
                "tools/make_timeline_svg.py.", "m")
    height = y + 24

    light = "\n".join(f"    --{k}: {lc};" for k, _, lc, _, _ in PHASES)
    dark = "\n".join(f"      --{k}: {dc};" for k, _, _, dc, _ in PHASES)
    fills = "\n".join(f"  .f-{k} {{ fill: var(--{k}); }}" for k, *_ in PHASES)
    style = f"""<style>
  svg {{
    --surface: #fcfcfb; --ink: #0b0b0b; --ink2: #52514e; --muted: #898781;
    --grid: #e1e0d9; --lane: #f0efec; --unit: {UNIT_FILL[0]};
{light}
  }}
  @media (prefers-color-scheme: dark) {{
    svg {{
      --surface: #1a1a19; --ink: #f0efec; --ink2: #c3c2b7; --muted: #898781;
      --grid: #2c2c2a; --lane: #242422; --unit: {UNIT_FILL[1]};
{dark}
    }}
  }}
  text {{ font-family: system-ui, -apple-system, "Segoe UI", sans-serif; }}
  .h {{ font-size: 20px; font-weight: 700; fill: var(--ink); }}
  .t {{ font-size: 13px; font-weight: 600; fill: var(--ink); }}
  .s {{ font-size: 12px; fill: var(--ink2); }}
  .v {{ font-size: 12px; fill: var(--ink); font-variant-numeric: tabular-nums; }}
  .m {{ font-size: 11px; fill: var(--muted); font-variant-numeric: tabular-nums; }}
  .in {{ font-size: 12px; font-weight: 600; fill: #0b0b0b; }}
{fills}
  .f-unit {{ fill: var(--unit); }}
  .lane {{ fill: var(--lane); }}
  .idle {{ fill: var(--lane); stroke: var(--grid); stroke-dasharray: 3 3; }}
  .grid {{ stroke: var(--grid); stroke-width: 1; }}
  .limit {{ stroke: var(--ink2); stroke-width: 1.5; stroke-dasharray: 5 4; }}
</style>"""
    phases_txt = ", ".join(f"{name} {ms(sums.get(key, 0))} ms" for key, name, *_ in PHASES if sums.get(key))
    busy_txt = ", ".join(f"{name} {ms(c)} ms" for name, c in hw[:-1])
    aria = (f"Ablauf eines Bildes über 20 ms: die CPU nacheinander {phases_txt}; "
            f"fertig nach {ms(end)} ms. Beschäftigt: {busy_txt}.")
    doc = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {height:.0f}" width="{WIDTH}" '
           f'height="{height:.0f}" role="img" aria-label="{escape(aria)}">',
           "<title>Ablauf eines Bildes</title>", style,
           f'<rect width="{WIDTH}" height="{height:.0f}" fill="var(--surface)"/>'] + parts + ["</svg>", ""]
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(doc))
    print(f"{out}: {WIDTH} x {height:.0f}; frame ends at {fmt(end)} cycles ({ms(end)} ms)")
    for name, c in rows:
        print(f"  {name:45s} {fmt(c):>10s}")
    for name, c in hw:
        print(f"  {name:45s} {fmt(c):>10s}")


if __name__ == "__main__":
    main()
