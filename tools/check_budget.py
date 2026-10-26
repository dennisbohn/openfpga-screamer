"""Check per-entity block RAM use and the IO registers of the memory pins in a Quartus fit report.

Usage: python check_budget.py <ap_core.fit.rpt>

Quartus can silently map a memory onto registers (0 M10K) or duplicate it (twice the
blocks) when the code is changed. This fails the build when an entity listed in
EXPECTED does not use exactly that many M10K.

The SDRAM, SRAM and PSRAM cram0 pins must take their signals straight from registers in
the IO cells (docs/speicher.md); otherwise their timing depends on the placement. Quartus
ignores a FAST_*_REGISTER assignment it cannot honour with only a warning, so this also
fails the build when a pin in IO_REGS lacks one of its registers in the pin tables.
"""
import sys

EXPECTED = {
    "|framebuffer:fb|": 128,
}

# pin name (without the bus index) -> registers it needs: in, out, oe
IO_REGS = {name: ("out",) for name in (
    "dram_a", "dram_ba", "dram_dqm", "dram_cke", "dram_ras_n", "dram_cas_n", "dram_we_n")}
IO_REGS.update({name: ("in", "out", "oe") for name in ("dram_dq",)})

COLUMNS = {"in": "Input Register", "out": "Output Register", "oe": "Output Enable Register"}


def check_m10k(lines):
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
    return ok


def pin_rows(lines, title):
    """Rows of one pin table as dicts, keyed by the header cells."""
    start = next(i for i, l in enumerate(lines) if l.startswith("; " + title))
    header = next(l for l in lines[start + 1:] if l.startswith("; Name"))
    cols = [c.strip() for c in header.split(";")]
    rows = []
    for l in lines[lines.index(header, start) + 2:]:
        if not l.startswith(";"):
            break
        cells = [c.strip() for c in l.split(";")]
        rows.append(dict(zip(cols, cells)))
    return rows


def check_io(lines):
    ok, seen = True, {}
    for row in pin_rows(lines, "Output Pins") + pin_rows(lines, "Bidir Pins"):
        name = row.get("Name", "")
        base = name.split("[")[0]
        if base not in IO_REGS:
            continue
        seen[base] = seen.get(base, 0) + 1
        missing = [k for k in IO_REGS[base] if row.get(COLUMNS[k]) != "yes"]
        if missing:
            ok = False
            print(f"IO register missing on {name}: {', '.join(COLUMNS[k] for k in missing)}")
    for base in IO_REGS:
        if base not in seen:
            ok = False
            print(f"IO pin {base} not found in the pin tables")
    print(f"IO registers on {sum(seen.values())} memory pins: {'ok' if ok else 'WRONG'}")
    return ok


def main():
    lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
    ok = check_m10k(lines)
    ok &= check_io(lines)
    sys.exit(0 if ok else 1)


main()
