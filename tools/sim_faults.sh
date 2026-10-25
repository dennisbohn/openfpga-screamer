#!/bin/sh
# Negative tests of the memory test (docs/speicher.md): sim/soc with one fault built into a
# memory model per run. The memory test must report exactly the tests named below, no
# more and no fewer; tb.cpp compares SYS_SIM and checks that the log reports a failure.
# The runs go in parallel, each with its own Verilator build.
# Usage: tools/sim_faults.sh [firmware.bin]     (default build/firmware/firmware.bin)
# Output: build/sim/faults/<name>.txt per run, a summary at the end; exit 1 if one failed.
#
# SYS_SIM bits (firmware/memtest.c): 4 program.bin in the SDRAM, 5 phase, 6 data bus, 7 bytes, 8 address,
# 9 patterns, 10 code, 11 refresh, 12 write status; SRAM 13..16 and cram0 17..20 data bus,
# bytes, address, patterns. All passing is 801FFFFF.
root=$(cd "$(dirname "$0")/.." && pwd)
fw=${1:-build/firmware/firmware.bin}
cd "$root" || exit 1
mkdir -p build/sim/faults

# name, defines, expected SYS_SIM
cases="
dqm_swap|-DSDRAM_FAULT_DQM_SWAP=1|801FFF7F
dq8_stuck0|-DSDRAM_FAULT_DQ_STUCK0=256|801FF80F
ba1_stuck0|-DSDRAM_FAULT_BA1_STUCK0=1|801FFEEF
row12_from_row11|-DSDRAM_FAULT_ROW_ALIAS_SRC=11 -DSDRAM_FAULT_ROW_ALIAS_DST=12|801FFEFF
drop_write|-DSDRAM_FAULT_DROP_WRITE_N=16000|801FFDFF
sram_ub_lb_swap|-DSRAM_FAULT_SWAP_UB_LB=1|801FBFFF
cram0_die1_dead|-DPSRAM_FAULT_DIE1_DEAD=1|8007FFFF
"
# Why exactly these bits:
#   dqm_swap          the SDRAM's byte masks swapped: only byte stores go wrong (bytes)
#   dq8_stuck0        DQ8 stuck at 0: every SDRAM test that moves data (program.bin,
#                     phase, data bus, bytes, address, patterns, code); write status stores
#                     600DF00D, which has bits 8 and 24 clear; the code test must not run
#                     what it could not read back; the boot loader stays with the results
#   ba1_stuck0        banks 2 and 3 land in 0 and 1: the address test, and program.bin,
#                     whose 15 KB from offset 3000000h reach bank 2 (+1000h) and overwrite
#                     bank 0; the boot loader stays with the results
#   row12_from_row11  rows with bit 12 != bit 11 land elsewhere: only the address test
#                     (offset 2000000h overwrites offset 0); the windows use rows 0 and
#                     1FFFh, program.bin rows from 1800h (both bits 1), the other tests
#                     rows with both bits 0
#   drop_write        one write beat lost in 16000: after program.bin (about 7300 beats)
#                     and the tests before the patterns (about 1400) the first loss falls
#                     into the patterns (about 11800 beats); later ones land in the test
#                     textures the program writes, which the rasterizer and its check share
#   sram_ub_lb_swap   the SRAM's byte enables swapped: only byte stores (SRAM bytes)
#   cram0_die1_dead   the second PSRAM die never answers: address (offset 800000h) and
#                     patterns (the windows of die 1)

echo "$cases" | {
    while IFS='|' read -r name defines expect; do
        [ -z "$name" ] && continue
        rm -f "build/sim/faults/$name.exit"
        ( SIM_DEFINES="$defines" tools/sim.sh soc "$fw" "build/sim/faults/$name" 80 "$expect" \
            > "build/sim/faults/$name.txt" 2>&1
          echo $? > "build/sim/faults/$name.exit" ) &
    done
    wait
}

failed=0
echo "$cases" | {
    while IFS='|' read -r name defines expect; do
        [ -z "$name" ] && continue
        code=$(cat "build/sim/faults/$name.exit" 2>/dev/null || echo "?")
        got=$(sed -n 's/^SYS_SIM = \(0x[0-9a-f]*\) .*/\1/p' "build/sim/faults/$name.txt" | head -n 1)
        if [ "$code" = "0" ]; then res=PASS; else res=FAIL; failed=1; fi
        printf '%-18s %s  SYS_SIM %s, expected 0x%s\n' "$name" "$res" "${got:-?}" "$expect"
    done
    exit $failed
}
