//
// Simulation top for the rasterizer alone (sim/raster/tb.cpp): raster.v and the framebuffer
// (both buffers), the drawing port shared as in soc.v (the rasterizer's while it is busy,
// else the testbench's); the registers, the setup unit's triangle and the texture line port
// are the testbench's
//

module raster_system (
    input   wire            clk,
    input   wire            reset,

    input   wire            wr,
    input   wire    [4:0]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            busy,
    output  wire    [31:0]  pixels,

    // the framebuffer port while the rasterizer is idle: a whole word written, or read (fb_q
    // the clock after)
    input   wire            tb_wr,
    input   wire            tb_buf,
    input   wire    [13:0]  tb_addr,
    input   wire    [31:0]  tb_data,
    output  wire    [31:0]  fb_q,

    output  wire            mem_valid,
    input   wire            mem_ready,
    output  wire    [25:5]  mem_addr,
    input   wire            mem_rsp_valid,
    input   wire    [31:0]  mem_rsp_data,

    input   wire            su_go,
    output  wire            su_accept,
    input   wire    [31:0]  su_v0, su_v1, su_v2, su_ref, su_tex, su_size,
    input   wire    [9:0]   su_draw,
    input   wire    [31:0]  su_a0, su_a1, su_a2,
    input   wire    [31:0]  su_dx0, su_dx1, su_dx2,
    input   wire    [31:0]  su_dy0, su_dy1, su_dy2
);

    wire        rs_fb_wr, rs_fb_buf;
    wire [13:0] rs_fb_addr;
    wire [31:0] rs_fb_data;
    wire [3:0]  rs_fb_be;
    wire [31:0] misses, busy_clocks, miss_clocks, edge_clocks, row_clocks, drain_clocks;
    wire [31:0] vid_q;

raster rs (
    .clk                ( clk ),
    .reset              ( reset ),
    .wr                 ( wr ),
    .wr_addr            ( wr_addr ),
    .wr_data            ( wr_data ),
    .busy               ( busy ),
    .pixels             ( pixels ),
    .misses             ( misses ),
    .busy_clocks        ( busy_clocks ),
    .miss_clocks        ( miss_clocks ),
    .edge_clocks        ( edge_clocks ),
    .row_clocks         ( row_clocks ),
    .drain_clocks       ( drain_clocks ),
    .fb_wr              ( rs_fb_wr ),
    .fb_buf             ( rs_fb_buf ),
    .fb_addr            ( rs_fb_addr ),
    .fb_data            ( rs_fb_data ),
    .fb_be              ( rs_fb_be ),
    .fb_q               ( fb_q ),
    .mem_valid          ( mem_valid ),
    .mem_ready          ( mem_ready ),
    .mem_addr           ( mem_addr ),
    .mem_rsp_valid      ( mem_rsp_valid ),
    .mem_rsp_data       ( mem_rsp_data ),
    .su_go              ( su_go ),
    .su_accept          ( su_accept ),
    .su_v0              ( su_v0 ),
    .su_v1              ( su_v1 ),
    .su_v2              ( su_v2 ),
    .su_ref             ( su_ref ),
    .su_tex             ( su_tex ),
    .su_size            ( su_size ),
    .su_draw            ( su_draw ),
    .su_a0              ( su_a0 ),
    .su_a1              ( su_a1 ),
    .su_a2              ( su_a2 ),
    .su_dx0             ( su_dx0 ),
    .su_dx1             ( su_dx1 ),
    .su_dx2             ( su_dx2 ),
    .su_dy0             ( su_dy0 ),
    .su_dy1             ( su_dy1 ),
    .su_dy2             ( su_dy2 )
);

framebuffer fb (
    .clk_sys            ( clk ),
    .wr_en              ( busy ? rs_fb_wr : tb_wr ),
    .wr_buf             ( busy ? rs_fb_buf : tb_buf ),
    .wr_addr            ( busy ? rs_fb_addr : tb_addr ),
    .wr_data            ( busy ? rs_fb_data : tb_data ),
    .wr_be              ( busy ? rs_fb_be : 4'b1111 ),
    .sys_rd_data        ( fb_q ),
    .clk_vid            ( clk ),
    .rd_buf             ( 1'b0 ),
    .rd_addr            ( 14'd0 ),
    .rd_data            ( vid_q )
);

endmodule
