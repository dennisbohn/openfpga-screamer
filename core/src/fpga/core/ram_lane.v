//
// One byte lane of a block RAM: 2^ADDR_W x 8 bits, two independent ports
//   port A (clk_a): read or write one byte, data one clock after the address;
//                   a read of the byte being written returns the new byte
//   port B (clk_b): the same
// Wider memories with byte enables are built from four of these (framebuffer.v,
// soc.v): one lane per byte, each with its own write enable.
//
// Quartus gets an explicit altsyncram: inferring a memory with byte enables builds
// registers instead of M10K, and inferring a second read port duplicates the blocks.
// The simulation gets the same behaviour from an array, starting from a poison byte so
// a lost write shows up.
//

module ram_lane #(
    parameter ADDR_W = 15
) (
    input   wire                clk_a,
    input   wire    [ADDR_W-1:0] addr_a,
    input   wire                we_a,
    input   wire    [7:0]       wdata_a,
    output  wire    [7:0]       q_a,

    input   wire                clk_b,
    input   wire    [ADDR_W-1:0] addr_b,
    input   wire                we_b,
    input   wire    [7:0]       wdata_b,
    output  wire    [7:0]       q_b
);

`ifdef VERILATOR

    reg [7:0]   mem [0:(1<<ADDR_W)-1];
    reg [7:0]   ra, rb;

    integer i;
    initial for(i = 0; i < (1<<ADDR_W); i = i + 1) mem[i] = 8'hA5;

always @(posedge clk_a) begin
    if(we_a) mem[addr_a] <= wdata_a;
    ra <= we_a ? wdata_a : mem[addr_a];
end

always @(posedge clk_b) begin
    if(we_b) mem[addr_b] <= wdata_b;
    rb <= we_b ? wdata_b : mem[addr_b];
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
    .widthad_a                      ( ADDR_W ),
    .numwords_a                     ( 1 << ADDR_W ),
    .width_b                        ( 8 ),
    .widthad_b                      ( ADDR_W ),
    .numwords_b                     ( 1 << ADDR_W ),
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
    .clock0         ( clk_a ),
    .address_a      ( addr_a ),
    .wren_a         ( we_a ),
    .data_a         ( wdata_a ),
    .q_a            ( q_a ),
    .clock1         ( clk_b ),
    .address_b      ( addr_b ),
    .wren_b         ( we_b ),
    .data_b         ( wdata_b ),
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
