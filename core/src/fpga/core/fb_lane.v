//
// One byte lane of the framebuffers: 32768 x 8 bits, 32 M10K
//   port A (clk_sys): read or write one byte, data one clock after the address;
//                     a read of the byte being written returns the new byte
//   port B (clk_vid): read only, data one clock after the address
//
// Quartus gets an explicit altsyncram: inferring this memory with byte enables builds
// registers instead of M10K, and inferring a second read port duplicates the blocks.
// The simulation gets the same behaviour from an array, starting from a poison byte so
// a lost write shows up.
//

module fb_lane (
    input   wire            clk_sys,
    input   wire    [14:0]  addr_a,
    input   wire            we_a,
    input   wire    [7:0]   wdata_a,
    output  wire    [7:0]   q_a,

    input   wire            clk_vid,
    input   wire    [14:0]  addr_b,
    output  wire    [7:0]   q_b
);

`ifdef VERILATOR

    reg [7:0]   mem [0:32767];
    reg [7:0]   ra, rb;

    integer i;
    initial for(i = 0; i < 32768; i = i + 1) mem[i] = 8'hA5;

always @(posedge clk_sys) begin
    if(we_a) mem[addr_a] <= wdata_a;
    ra <= we_a ? wdata_a : mem[addr_a];
end

always @(posedge clk_vid) begin
    rb <= mem[addr_b];
end

    assign q_a = ra;
    assign q_b = rb;

`else

altsyncram #(
    .operation_mode                 ( "BIDIR_DUAL_PORT" ),
    .intended_device_family         ( "Cyclone V" ),
    .lpm_type                       ( "altsyncram" ),
    .ram_block_type                 ( "M10K" ),
    .power_up_uninitialized         ( "FALSE" ),
    .width_a                        ( 8 ),
    .widthad_a                      ( 15 ),
    .numwords_a                     ( 32768 ),
    .width_b                        ( 8 ),
    .widthad_b                      ( 15 ),
    .numwords_b                     ( 32768 ),
    .width_byteena_a                ( 1 ),
    .width_byteena_b                ( 1 ),
    .outdata_reg_a                  ( "UNREGISTERED" ),
    .outdata_reg_b                  ( "UNREGISTERED" ),
    .address_reg_b                  ( "CLOCK1" ),
    .indata_reg_b                   ( "CLOCK1" ),
    .wrcontrol_wraddress_reg_b      ( "CLOCK1" ),
    .clock_enable_input_a           ( "BYPASS" ),
    .clock_enable_input_b           ( "BYPASS" ),
    .clock_enable_output_a          ( "BYPASS" ),
    .clock_enable_output_b          ( "BYPASS" ),
    .read_during_write_mode_port_a  ( "NEW_DATA_NO_NBE_READ" ),
    .read_during_write_mode_port_b  ( "NEW_DATA_NO_NBE_READ" )
) ram (
    .clock0         ( clk_sys ),
    .address_a      ( addr_a ),
    .wren_a         ( we_a ),
    .data_a         ( wdata_a ),
    .q_a            ( q_a ),
    .clock1         ( clk_vid ),
    .address_b      ( addr_b ),
    .wren_b         ( 1'b0 ),
    .data_b         ( 8'h00 ),
    .q_b            ( q_b ),
    .aclr0          ( 1'b0 ),
    .aclr1          ( 1'b0 ),
    .addressstall_a ( 1'b0 ),
    .addressstall_b ( 1'b0 ),
    .byteena_a      ( 1'b1 ),
    .byteena_b      ( 1'b1 ),
    .clocken0       ( 1'b1 ),
    .clocken1       ( 1'b1 ),
    .clocken2       ( 1'b1 ),
    .clocken3       ( 1'b1 ),
    .rden_a         ( 1'b1 ),
    .rden_b         ( 1'b1 ),
    .eccstatus      ( )
);

`endif

endmodule
