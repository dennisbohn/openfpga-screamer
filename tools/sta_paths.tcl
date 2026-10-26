# The worst setup paths of clk_74a in the slow corners, for tools/build.sh when timing
# fails: run with quartus_sta -t in core/src/fpga after a compile.
project_open ap_core
create_timing_netlist -model slow
read_sdc
update_timing_netlist
foreach_in_collection p [get_timing_paths -setup -npaths 12 -to_clock clk_74a] {
    post_message -type info [format "path %7.3f ns  %s  ->  %s" \
        [get_path_info $p -slack] \
        [get_node_info -name [get_path_info $p -from]] \
        [get_node_info -name [get_path_info $p -to]]]
}
delete_timing_netlist
project_close
