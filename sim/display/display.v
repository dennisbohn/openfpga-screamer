//
// Simulation top: fb_bridge + framebuffer + video_out wired as in core_top, plus a
// testbench override of the framebuffer's drawing port (tb_sys = 1) to check reads
// and byte-enable writes
//

module display (
    input   wire            clk_74a,
    input   wire            clk_vid,
    input   wire            reset_n,

    input   wire            bridge_wr,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,
    input   wire            swap_button,

    input   wire            tb_sys,
    input   wire            tb_we,
    input   wire            tb_buf,
    input   wire    [13:0]  tb_addr,
    input   wire    [3:0]   tb_be,
    input   wire    [31:0]  tb_wdata,
    output  wire    [31:0]  sys_rd_data,

    output  wire    [23:0]  vid_rgb,
    output  wire            vid_de,
    output  wire            vid_hs,
    output  wire            vid_vs,
    output  wire            front
);

    wire            fb_wr_en, fb_wr_buf;
    wire    [13:0]  fb_wr_addr;
    wire    [31:0]  fb_wr_data;
    wire            pal_wr, pal_buf;
    wire    [7:0]   pal_addr;
    wire    [23:0]  pal_data;
    wire            swap_req_toggle, swap_ack_toggle;
    wire            fb_rd_buf;
    wire            frame_toggle;       // not checked here
    wire    [13:0]  fb_rd_addr;
    wire    [31:0]  fb_rd_data;

fb_bridge fbb (
    .clk_74a        ( clk_74a ),
    .bridge_wr      ( bridge_wr ),
    .bridge_addr    ( bridge_addr ),
    .bridge_wr_data ( bridge_wr_data ),
    .swap_button    ( swap_button ),
    .fb_wr_en       ( fb_wr_en ),
    .fb_wr_buf      ( fb_wr_buf ),
    .fb_wr_addr     ( fb_wr_addr ),
    .fb_wr_data     ( fb_wr_data ),
    .pal_wr         ( pal_wr ),
    .pal_buf        ( pal_buf ),
    .pal_addr       ( pal_addr ),
    .pal_data       ( pal_data ),
    .swap_req_toggle( swap_req_toggle ),
    .swap_ack_toggle( swap_ack_toggle )
);

framebuffer fb (
    .clk_sys        ( clk_74a ),
    .wr_en          ( tb_sys ? tb_we : fb_wr_en ),
    .wr_buf         ( tb_sys ? tb_buf : fb_wr_buf ),
    .wr_addr        ( tb_sys ? tb_addr : fb_wr_addr ),
    .wr_data        ( tb_sys ? tb_wdata : fb_wr_data ),
    .wr_be          ( tb_sys ? tb_be : 4'b1111 ),
    .sys_rd_data    ( sys_rd_data ),
    .clk_vid        ( clk_vid ),
    .rd_buf         ( fb_rd_buf ),
    .rd_addr        ( fb_rd_addr ),
    .rd_data        ( fb_rd_data )
);

video_out vo (
    .clk_74a        ( clk_74a ),
    .pal_wr         ( pal_wr ),
    .pal_buf        ( pal_buf ),
    .pal_addr       ( pal_addr ),
    .pal_data       ( pal_data ),
    .swap_req_toggle( swap_req_toggle ),
    .swap_ack_toggle( swap_ack_toggle ),
    .front          ( front ),
    .frame_toggle   ( frame_toggle ),
    .fb_rd_buf      ( fb_rd_buf ),
    .fb_rd_addr     ( fb_rd_addr ),
    .fb_rd_data     ( fb_rd_data ),
    .clk_vid        ( clk_vid ),
    .reset_n        ( reset_n ),
    .vid_rgb        ( vid_rgb ),
    .vid_de         ( vid_de ),
    .vid_hs         ( vid_hs ),
    .vid_vs         ( vid_vs )
);

endmodule
