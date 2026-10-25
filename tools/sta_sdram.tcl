# Check the SDRAM IO timing of the compiled project in every corner (docs/speicher.md).
# Run in core/src/fpga after the compile: quartus_sta -t sta_sdram.tcl
#
# The four transfers between clk_74a and dram_clk_pin (output setup and hold, read setup
# and hold) must each have paths and a slack of at least 0.5 ns. A read hold above 5 ns
# means the check is vacuous (a wrong multicycle), which fails as well. These checks
# vanish silently if the clocks end up in asynchronous groups, hence the path count.

project_open ap_core
create_timing_netlist
read_sdc
update_timing_netlist

set min_slack 0.5
set max_read_hold 5.0
set failed 0

proc check {from to kind name} {
    global min_slack max_read_hold failed
    if {$kind == "setup"} {
        set r [report_timing -from_clock $from -to_clock $to -setup -npaths 1 -detail summary -quiet]
    } else {
        set r [report_timing -from_clock $from -to_clock $to -hold -npaths 1 -detail summary -quiet]
    }
    set paths [lindex $r 0]
    set slack [lindex $r 1]
    set verdict "ok"
    if {$paths == 0} {
        set verdict "NO PATHS"
        set failed 1
    } elseif {$slack < $min_slack} {
        set verdict "TOO SMALL"
        set failed 1
    } elseif {$name == "read hold" && $slack > $max_read_hold} {
        set verdict "VACUOUS"
        set failed 1
    }
    return [format "%-12s %8s ns  %s" $name $slack $verdict]
}

foreach_in_collection op [get_available_operating_conditions] {
    set_operating_conditions $op
    update_timing_netlist
    post_message -type info "SDRAM IO, [get_operating_conditions_info -display_name $op]:"
    post_message -type info "  [check clk_74a dram_clk_pin setup {output setup}]"
    post_message -type info "  [check clk_74a dram_clk_pin hold {output hold}]"
    post_message -type info "  [check dram_clk_pin clk_74a setup {read setup}]"
    post_message -type info "  [check dram_clk_pin clk_74a hold {read hold}]"
}

delete_timing_netlist
project_close
if {$failed} {
    post_message -type error "SDRAM IO timing check failed"
    qexit -error
}
