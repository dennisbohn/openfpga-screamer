#!/bin/sh
# Compile the Quartus project in core/src/fpga inside Docker (Quartus Prime Lite 18.1.1)
# and write the bit-reversed bitstream the Pocket loads to core/output/bitstream.rbf_r.
# If hold timing fails, the fitter is run again with the next seed,
# up to five tries, and the seed that met timing is left in ap_core.qsf: a few registers
# in front of the FPU's DSP blocks sit within 50 ps of hold, depending on placement.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
fpga="$root/core/src/fpga"
qsf="$fpga/ap_core.qsf"
image="raetro/quartus:pocket"
tries=5

# Git Bash on Windows: hand Docker a Windows path and stop MSYS rewriting /build.
if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$fpga")
else
    mount=$fpga
fi

mkdir -p "$root/build"
log="$root/build/quartus.log"

# worst slack of each check across all clocks, from the timing summary
worst() {
    awk '/^Type/ { split($0, t, "Model "); split(t[2], k, " ") }
         /^Slack/ { if (!(k[1] in min) || $3 < min[k[1]]) { min[k[1]] = $3; at[k[1]] = t[2] } }
         END { for (c in min) printf "Worst %-22s: %s ns (%s)%s\n", c, min[c], at[c], min[c] < 0 ? "  <-- TIMING FAILED" : "" }' \
        "$fpga/output_files/ap_core.sta.summary"
}

seed=$(sed -n 's/^set_global_assignment -name SEED \([0-9]*\).*/\1/p' "$qsf")
seed=${seed:-1}
try=1
while :; do
    if ! MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/build" -w /build "$image" \
        quartus_sh --flow compile ap_core > "$log" 2>&1; then
        grep -E "^Error" "$log" | head -20
        echo "Build failed, full log: $log"
        exit 1
    fi
    if ! worst | grep -q "TIMING FAILED"; then
        break
    fi
    # a setup path that is too slow does not get fast with another seed: show the worst ones
    if worst | grep -q "Setup.*TIMING FAILED"; then
        worst
        cp "$root/tools/sta_paths.tcl" "$fpga/sta_paths.tcl"
        MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/build" -w /build "$image" \
            quartus_sta -t sta_paths.tcl > "$root/build/sta_paths.log" 2>&1 || true
        rm -f "$fpga/sta_paths.tcl"
        grep -E "Info: path" "$root/build/sta_paths.log" | sed 's/^Info: //'
        echo "Setup timing fails (seed $seed): see the paths above, full log build/sta_paths.log"
        exit 1
    fi
    if [ "$try" -ge "$tries" ]; then
        echo "Timing still fails after $tries seeds:"
        worst
        exit 1
    fi
    echo "Timing fails with fitter seed $seed, trying seed $((seed + 1))"
    seed=$((seed + 1))
    try=$((try + 1))
    sed -i "s/^set_global_assignment -name SEED [0-9]*/set_global_assignment -name SEED $seed/" "$qsf"
done
grep -E "Quartus Prime Shell was successful" "$log" | tail -1
echo "fitter seed $seed"

python -I "$root/tools/reverse_rbf.py" "$fpga/output_files/ap_core.rbf" "$root/core/output/bitstream.rbf_r"

python -I "$root/tools/check_budget.py" "$fpga/output_files/ap_core.fit.rpt" || {
    echo "Block RAM or memory IO registers are not as they should be (see tools/check_budget.py)"; exit 1; }
# the SDRAM IO transfers in every corner (tools/sta_sdram.tcl)
cp "$root/tools/sta_sdram.tcl" "$fpga/sta_sdram.tcl"
if ! MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/build" -w /build "$image" \
    quartus_sta -t sta_sdram.tcl > "$root/build/sta_sdram.log" 2>&1; then
    rm -f "$fpga/sta_sdram.tcl"
    grep -E "SDRAM IO|^Info:   |Error" "$root/build/sta_sdram.log" | head -30
    echo "SDRAM IO timing check failed, full log: build/sta_sdram.log"
    exit 1
fi
rm -f "$fpga/sta_sdram.tcl"
# one line: the worst of each SDRAM IO check over the four corners
awk '/^Info:   [a-z]+ [a-z]+ +-?[0-9.]+ ns/ { k = $2 " " $3; if (!(k in m) || $4 < m[k]) m[k] = $4; n[k] = 1; o[++c] = k }
     END { printf "SDRAM IO"; for (i = 1; i <= c; i++) if (n[o[i]]) { printf "%s %s %+.3f", (i > 1 ? "," : ""), o[i], m[o[i]]; n[o[i]] = 0 }
           print " (worst of the corners)" }' "$root/build/sta_sdram.log"
# the summary: build time, resources, the worst slack of each check, the logic of each unit,
# each against the build before (tools/buildsum.py)
python -I "$root/tools/buildsum.py" || worst
