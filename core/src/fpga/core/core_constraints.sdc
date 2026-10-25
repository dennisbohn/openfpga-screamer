#
# user core constraints
#
# put your clock groups in here as well as any net assignments
#

set_clock_groups -asynchronous \
 -group { bridge_spiclk } \
 -group { clk_74a } \
 -group { clk_74b } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[1].gpll~PLL_OUTPUT_COUNTER|divclk } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[2].gpll~PLL_OUTPUT_COUNTER|divclk } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[3].gpll~PLL_OUTPUT_COUNTER|divclk }

#
# SDRAM (docs/speicher.md). dram_clk is the SDRAM PLL's output (pll_sd) forwarded through a
# DDIO register. clk_74a, that PLL output and dram_clk_pin are one synchronous domain:
# never put them into separate asynchronous groups above, or every SDRAM IO check below
# disappears without a warning (tools/sta_sdram.tcl fails the build then).
#
create_generated_clock -name dram_clk_pin \
    -source [get_pins {ic|pll_sd|altera_pll_i|cyclonev_pll|counter[0].output_counter|divclk}] \
    [get_ports {dram_clk}]

# chip timing (AS4C32M16MSA at CL2: setup 2.0 / hold 1.0 ns, tAC 6.0 / tOH 2.5 ns), plus
# 0.3 ns of board delay each way
set dram_out [get_ports {dram_a[*] dram_ba[*] dram_dqm[*] dram_cke dram_ras_n dram_cas_n dram_we_n dram_dq[*]}]
set_output_delay -clock dram_clk_pin -max 2.3 $dram_out
set_output_delay -clock dram_clk_pin -min -1.3 $dram_out
set_input_delay -clock dram_clk_pin -max 6.6 [get_ports {dram_dq[*]}]
set_input_delay -clock dram_clk_pin -min 2.5 [get_ports {dram_dq[*]}]

# sdram.v takes a read halfword READ_LATENCY = CL + 1 = 3 clocks after the READ, one clock
# after the default capture edge: setup multicycle READ_LATENCY - CL + 1 = 2. No hold
# multicycle, so the hold check stays at the edge before (with -hold 1 it means nothing).
set_multicycle_path -setup -end 2 -from [get_clocks dram_clk_pin] -to [get_clocks clk_74a]

# PLL output jitter on a regular IO (Cyclone V datasheet tOUTPJ_IO, 0.88 ns p-p) beyond what
# Quartus derives
set_clock_uncertainty -add -from [get_clocks clk_74a] -to [get_clocks dram_clk_pin] 0.3
set_clock_uncertainty -add -from [get_clocks dram_clk_pin] -to [get_clocks clk_74a] 0.3

#
# SRAM and PSRAM cram0: asynchronous, timed by the controllers in whole clocks
# (sram.v, ip/psram.sv). Bound the pin paths, which the IO registers keep short anyway.
#
set async_out [get_ports {sram_a[*] sram_oe_n sram_we_n sram_ub_n sram_lb_n sram_dq[*] \
    cram0_a[*] cram0_adv_n cram0_ce0_n cram0_ce1_n cram0_oe_n cram0_we_n cram0_ub_n cram0_lb_n cram0_dq[*]}]
set async_in [get_ports {sram_dq[*] cram0_dq[*]}]
set_max_delay -to $async_out 10.0
set_min_delay -to $async_out 0.0
set_max_delay -from $async_in 10.0
set_min_delay -from $async_in 0.0

