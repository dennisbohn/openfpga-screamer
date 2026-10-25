#!/bin/sh
# Generate the SDRAM clock PLL with Intel's ip-generate, in the Quartus image tools/build.sh
# uses (docs/speicher.md): integer mode, 74.25 MHz in and out (VCO 742.5 MHz), 10 ns phase
# asked for (9932 ps = 265.5 degrees come out), dynamic phase shift ports, locked output.
# Writes core/src/fpga/core/pll_sdram/ and core/src/fpga/core/pll_sdram.qip.
# Usage: tools/gen_pll_sdram.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
core="$root/core/src/fpga/core"
image="raetro/quartus:pocket"

# Git Bash on Windows: hand Docker a Windows path and stop MSYS rewriting /build.
if command -v cygpath >/dev/null 2>&1; then
    mount=$(cygpath -w "$core")
else
    mount=$core
fi

rm -rf "$core/pll_gen"
if ! MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/build" -w /build "$image" sh -c '
    mkdir -p /build/pll_gen &&
    /opt/intelFPGA/quartus/sopc_builder/bin/ip-generate \
        --output-directory=/build/pll_gen/pll_sdram \
        --file-set=QUARTUS_SYNTH \
        --report-file=qip:/build/pll_gen/pll_sdram.qip \
        --component-name=altera_pll \
        --output-name=pll_sdram \
        --system-info=DEVICE_FAMILY="Cyclone V" \
        --system-info=DEVICE=5CEBA4F23C8 \
        --system-info=DEVICE_SPEEDGRADE=8 \
        --component-parameter=gui_device_speed_grade=8 \
        --component-parameter=gui_pll_mode="Integer-N PLL" \
        --component-parameter=gui_reference_clock_frequency=74.25 \
        --component-parameter=gui_operation_mode=normal \
        --component-parameter=gui_use_locked=true \
        --component-parameter=gui_number_of_clocks=1 \
        --component-parameter=gui_output_clock_frequency0=74.25 \
        --component-parameter=gui_ps_units0=ps \
        --component-parameter=gui_phase_shift0=10000 \
        --component-parameter=gui_duty_cycle0=50 \
        --component-parameter=gui_en_dps_ports=true \
        --component-parameter=gui_pll_auto_reset=On \
        > /build/pll_gen/gen.log 2>&1'; then
    tail -40 "$core/pll_gen/gen.log"
    echo "ip-generate failed"
    exit 1
fi
rm -rf "$core/pll_sdram"
mv "$core/pll_gen/pll_sdram" "$core/pll_sdram"
mv "$core/pll_gen/pll_sdram.qip" "$core/pll_sdram.qip"
rm -rf "$core/pll_gen"
grep -E "output_clock_frequency0|phase_shift0|pll_vco|m_cnt|c_cnt_hi_div0|c_cnt_lo_div0" \
    "$core/pll_sdram/pll_sdram.v" || true
