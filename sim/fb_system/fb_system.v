//
// Simulation top: fb_psram + video_out wired as in core_top, with the PSRAM model on cram1
//

module fb_system (
    input   wire            clk_74a,
    input   wire            clk_vid,
    input   wire            reset_n,

    input   wire            bridge_wr,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,

    output  wire    [23:0]  vid_rgb,
    output  wire            vid_de,
    output  wire            vid_hs,
    output  wire            vid_vs,
    output  wire            error,
    output  wire            psram_timing_error
);

    wire            pal_wr;
    wire    [7:0]   pal_addr;
    wire    [23:0]  pal_data;
    wire            lb_wr;
    wire    [8:0]   lb_addr;
    wire    [15:0]  lb_data;
    wire            line_req_toggle;
    wire    [7:0]   line_req_y;

    wire    [21:16] cram_a;
    wire    [15:0]  cram_dq;
    wire            cram_adv_n, cram_ce0_n, cram_ce1_n, cram_oe_n, cram_we_n, cram_ub_n, cram_lb_n;
    wire            cram_clk, cram_cre;

fb_psram fbp (
    .clk_74a        ( clk_74a ),
    .bridge_wr      ( bridge_wr ),
    .bridge_addr    ( bridge_addr ),
    .bridge_wr_data ( bridge_wr_data ),
    .pal_wr         ( pal_wr ),
    .pal_addr       ( pal_addr ),
    .pal_data       ( pal_data ),
    .line_req_toggle( line_req_toggle ),
    .line_req_y     ( line_req_y ),
    .lb_wr          ( lb_wr ),
    .lb_addr        ( lb_addr ),
    .lb_data        ( lb_data ),
    .error          ( error ),
    .cram_a         ( cram_a ),
    .cram_dq        ( cram_dq ),
    .cram_wait      ( 1'b0 ),
    .cram_clk       ( cram_clk ),
    .cram_adv_n     ( cram_adv_n ),
    .cram_cre       ( cram_cre ),
    .cram_ce0_n     ( cram_ce0_n ),
    .cram_ce1_n     ( cram_ce1_n ),
    .cram_oe_n      ( cram_oe_n ),
    .cram_we_n      ( cram_we_n ),
    .cram_ub_n      ( cram_ub_n ),
    .cram_lb_n      ( cram_lb_n )
);

video_out vo (
    .clk_74a        ( clk_74a ),
    .pal_wr         ( pal_wr ),
    .pal_addr       ( pal_addr ),
    .pal_data       ( pal_data ),
    .lb_wr          ( lb_wr ),
    .lb_addr        ( lb_addr ),
    .lb_data        ( lb_data ),
    .error          ( error ),
    .clk_vid        ( clk_vid ),
    .reset_n        ( reset_n ),
    .line_req_toggle( line_req_toggle ),
    .line_req_y     ( line_req_y ),
    .vid_rgb        ( vid_rgb ),
    .vid_de         ( vid_de ),
    .vid_hs         ( vid_hs ),
    .vid_vs         ( vid_vs )
);

psram_model cram1 (
    .clk            ( clk_74a ),
    .a              ( cram_a ),
    .dq             ( cram_dq ),
    .adv_n          ( cram_adv_n ),
    .ce0_n          ( cram_ce0_n ),
    .oe_n           ( cram_oe_n ),
    .we_n           ( cram_we_n ),
    .ub_n           ( cram_ub_n ),
    .lb_n           ( cram_lb_n ),
    .timing_error   ( psram_timing_error )
);

endmodule
