#!/bin/sh
# Compile the Quartus project in core/src/fpga inside Docker (Quartus Prime Lite 18.1.1)
# and write the bit-reversed bitstream the Pocket loads to core/output/bitstream.rbf_r.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
fpga="$root/core/src/fpga"
image="raetro/quartus:pocket"

# Git Bash on Windows: hand Docker a Windows path and stop MSYS rewriting /build.
if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$fpga")
else
    mount=$fpga
fi

mkdir -p "$root/build"
log="$root/build/quartus.log"
if ! MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/build" -w /build "$image" \
    quartus_sh --flow compile ap_core > "$log" 2>&1; then
    grep -E "^Error" "$log" | head -20
    echo "Build failed, full log: $log"
    exit 1
fi
grep -E "Quartus Prime Shell was successful" "$log" | tail -1

python -I "$root/tools/reverse_rbf.py" "$fpga/output_files/ap_core.rbf" "$root/core/output/bitstream.rbf_r"

# Short summary: resource usage and the worst slack of each check across all clocks.
grep -E "Logic utilization|Total RAM Blocks|Total DSP Blocks|Total PLLs" \
    "$fpga/output_files/ap_core.fit.summary"
python -I "$root/tools/check_budget.py" "$fpga/output_files/ap_core.fit.rpt" || {
    echo "Block RAM of an entity is not what it should be (see tools/check_budget.py)"; exit 1; }
awk '/^Type/ { split($0, t, "Model "); split(t[2], k, " ") }
     /^Slack/ { if (!(k[1] in min) || $3 < min[k[1]]) { min[k[1]] = $3; at[k[1]] = t[2] } }
     END { for (c in min) printf "Worst %-22s: %s ns (%s)%s\n", c, min[c], at[c], min[c] < 0 ? "  <-- TIMING FAILED" : "" }' \
    "$fpga/output_files/ap_core.sta.summary"
