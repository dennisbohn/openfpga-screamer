"""Check per-entity block RAM use in a Quartus fit report.

Usage: python check_budget.py <ap_core.fit.rpt>

Quartus can silently map a memory onto registers (0 M10K) or duplicate it (twice the
blocks) when the code is changed. This fails the build when an entity listed in
EXPECTED does not use exactly that many M10K.
"""
import sys

EXPECTED = {
    "|framebuffer:fb|": 128,
}


def main():
    lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith("; Fitter Resource Utilization by Entity"))
    header = next(l for l in lines[start:] if l.startswith("; Compilation Hierarchy Node"))
    cols = [c.strip() for c in header.split(";")]
    m10k = cols.index("M10Ks")
    found, ok = {}, True
    for l in lines[start:]:
        cells = [c.strip() for c in l.split(";")]
        if len(cells) <= m10k:
            continue
        for name in EXPECTED:
            if cells[1] == name:
                found[name] = int(cells[m10k].split()[0])
    for name, want in EXPECTED.items():
        got = found.get(name)
        status = "ok" if got == want else "WRONG"
        ok &= got == want
        print(f"M10K {name.strip('|')}: {got} (expected {want}) {status}")
    sys.exit(0 if ok else 1)


main()
