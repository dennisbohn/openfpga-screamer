//
// Two 320x200 8-bit framebuffers in block RAM: video_out shows one while the other is drawn
//
// Four byte lanes (ram_lane.v) of 2 x 16384 words, 4 pixels a word, the leftmost in
// [31:24] (lane 3); the buffer number is the top address bit. 128 M10K.
//   drawing port (clk_sys): read or write a word, byte enables for single pixels; read
//                           data one clock after the address (new data on a write)
//   video port (clk_vid):   read only, data one clock after the address
// The drawing side has to read too: the game's see-through effects (smoke, glass,
// shadows, the radar) blend with what is already on the screen.
//

module framebuffer (
    input   wire            clk_sys,
    input   wire            wr_en,
    input   wire            wr_buf,
    input   wire    [13:0]  wr_addr,        // word 0..15999, also the read address
    input   wire    [31:0]  wr_data,
    input   wire    [3:0]   wr_be,          // [3] = leftmost pixel, [31:24]
    output  wire    [31:0]  sys_rd_data,

    input   wire            clk_vid,
    input   wire            rd_buf,
    input   wire    [13:0]  rd_addr,
    output  wire    [31:0]  rd_data
);

    wire [14:0] addr_a = {wr_buf, wr_addr};
    wire [14:0] addr_b = {rd_buf, rd_addr};

    genvar lane;
    generate
        for(lane = 0; lane < 4; lane = lane + 1) begin : lanes
            ram_lane #(.ADDR_W(15)) ram (
                .clk_a      ( clk_sys ),
                .addr_a     ( addr_a ),
                .we_a       ( wr_en & wr_be[lane] ),
                .wdata_a    ( wr_data[lane*8 +: 8] ),
                .q_a        ( sys_rd_data[lane*8 +: 8] ),
                .clk_b      ( clk_vid ),
                .addr_b     ( addr_b ),
                .we_b       ( 1'b0 ),
                .wdata_b    ( 8'h00 ),
                .q_b        ( rd_data[lane*8 +: 8] )
            );
        end
    endgenerate

endmodule
