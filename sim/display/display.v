//
// Simulation top: fb_bridge + framebuffer + video_out wired as in core_top
//

module display (
    input   wire            clk_74a,
    input   wire            clk_vid,
    input   wire            reset_n,

    input   wire            bridge_wr,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,
    input   wire            swap_button,

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
    .wr_en          ( fb_wr_en ),
    .wr_buf         ( fb_wr_buf ),
    .wr_addr        ( fb_wr_addr ),
    .wr_data        ( fb_wr_data ),
    .wr_be          ( 4'b1111 ),
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
