"""The last Quartus build in a few lines: build time, logic, block RAM and DSP, the worst slack
of each timing check and the logic of each unit, each with its change against the build before
(kept in build/last_build.json). tools/build.sh prints it at the end.

Usage: python tools/buildsum.py [--keep]     (--keep: do not replace build/last_build.json)
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_budget_svg as mb  # noqa: E402

OUT = os.path.join(HERE, "..", "core", "src", "fpga", "output_files")
LAST = os.path.join(HERE, "..", "build", "last_build.json")
CHECKS = ("Setup", "Hold", "Recovery", "Removal", "Minimum Pulse Width")


def slacks(path):
    """{check: worst slack in ns} over all clocks and corners of the timing summary"""
    worst, kind = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"Type\s*:.*?Model\s+(.+?)\s+'", line)
        if m:
            kind = next((c for c in CHECKS if m.group(1).startswith(c)), None)
        m = re.match(r"Slack\s*:\s*(-?[\d.]+)", line)
        if m and kind:
            worst[kind] = min(worst.get(kind, 1e9), float(m.group(1)))
    return worst


def elapsed(path):
    """the whole flow's time in seconds, from the flow report's elapsed time table"""
    secs = 0
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r";\s*(Analysis & Synthesis|Fitter|Assembler|Timing Analyzer)\s*;\s*(\d+):(\d+):(\d+)", line)
        if m:
            secs += int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4))
    return secs


def de(n, sign=False):
    """an integer with thousands separators"""
    return (f"{n:+,}" if sign else f"{n:,}")


def delta(now, before, fmt=None):
    if before is None or now == before:
        return ""
    return " (" + (fmt.format(now - before) if fmt else de(now - before, True)) + ")"


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    nodes = mb.read_report(os.path.join(OUT, "ap_core.fit.rpt"))
    units = {c[0]: c[2] for c in mb.components(nodes)}
    top = nodes[mb.TOP]
    now = {"alm": round(top[0]), "m10k": top[1], "dsp": top[2], "units": units,
           "slack": slacks(os.path.join(OUT, "ap_core.sta.summary")),
           "secs": elapsed(os.path.join(OUT, "ap_core.flow.rpt"))}
    last = json.load(open(LAST)) if os.path.exists(LAST) else {}
    cap = mb.CAPACITY
    t, lt = now["secs"], last.get("secs")
    print(f"build {t // 60}:{t % 60:02d} min" + (f" ({(t - lt):+d} s)" if lt and lt != t else "") +
          f" | logic {de(now['alm'])} ALMs ({round(100 * now['alm'] / cap['alm'])} %)" +
          delta(now["alm"], last.get("alm")) +
          f" | M10K {now['m10k']} ({round(100 * now['m10k'] / cap['m10k'])} %)" + delta(now["m10k"], last.get("m10k")) +
          f" | DSP {now['dsp']}" + delta(now["dsp"], last.get("dsp")))
    ls = last.get("slack", {})
    print("slack " + ", ".join(f"{k.split()[0].lower()} {v:+.3f}" + delta(v, ls.get(k), "{:+.3f}")
                                 for k, v in now["slack"].items()) +
          ("   <-- TIMING FAILED" if min(now["slack"].values(), default=0) < 0 else ""))
    lu = last.get("units", {})
    print("units " + ", ".join(f"{k.split(' (')[0].split(':')[0]} {de(v)}" + delta(v, lu.get(k))
                                 for k, v in sorted(units.items(), key=lambda kv: -kv[1])))
    if "--keep" not in sys.argv:
        os.makedirs(os.path.dirname(LAST), exist_ok=True)
        json.dump(now, open(LAST, "w"))


main()
