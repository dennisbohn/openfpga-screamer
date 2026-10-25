//
// Simulation top for step 1: soc + framebuffer + video_out wired as in core_top, and a
// model of the core side of the APF data slot table (the table core_bridge_cmd keeps)
//

module soc_system (
    input   wire            clk_74a,
    input   wire            clk_vid,
    input   wire            reset_n,

    input   wire            bridge_wr,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,
    output  wire    [31:0]  bridge_log_rd_data,

    input   wire    [31:0]  cont1_key,

    output  wire    [23:0]  vid_rgb,
    output  wire            vid_de,
    output  wire            vid_hs,
    output  wire            vid_vs,

    output  wire    [31:0]  sim_status,
    output  wire    [31:0]  log_slot_size      // size word of the debug log's slot table entry
);

    wire            fb_wr_en, fb_buf;
    wire    [13:0]  fb_addr;
    wire    [31:0]  fb_wr_data, fb_rd_data;
    wire    [3:0]   fb_wr_be;
    wire            pal_wr, pal_buf;
    wire    [7:0]   pal_addr;
    wire    [23:0]  pal_data;
    wire            swap_req_toggle, swap_ack_toggle, frame_toggle, front;
    wire            fb_rd_buf;
    wire    [13:0]  fb_rd_addr;
    wire    [31:0]  vid_fb_data;
    wire    [9:0]   dt_addr;
    wire            dt_wren;
    wire    [31:0]  dt_data;
    reg     [31:0]  dt_q;

soc soc (
    .clk                ( clk_74a ),
    .reset_n            ( reset_n ),
    .bridge_wr          ( bridge_wr ),
    .bridge_addr        ( bridge_addr ),
    .bridge_wr_data     ( bridge_wr_data ),
    .bridge_log_rd_data ( bridge_log_rd_data ),
    .cont1_key          ( cont1_key ),
    .cont1_joy          ( 32'd0 ),
    .cont1_trig         ( 16'd0 ),
    .fb_wr_en           ( fb_wr_en ),
    .fb_buf             ( fb_buf ),
    .fb_addr            ( fb_addr ),
    .fb_wr_data         ( fb_wr_data ),
    .fb_wr_be           ( fb_wr_be ),
    .fb_rd_data         ( fb_rd_data ),
    .pal_wr             ( pal_wr ),
    .pal_buf            ( pal_buf ),
    .pal_addr           ( pal_addr ),
    .pal_data           ( pal_data ),
    .swap_req_toggle    ( swap_req_toggle ),
    .swap_ack_toggle    ( swap_ack_toggle ),
    .frame_toggle       ( frame_toggle ),
    .dt_addr            ( dt_addr ),
    .dt_wren            ( dt_wren ),
    .dt_data            ( dt_data ),
    .dt_q               ( dt_q ),
    .sim_status         ( sim_status )
);

framebuffer fb (
    .clk_sys            ( clk_74a ),
    .wr_en              ( fb_wr_en ),
    .wr_buf             ( fb_buf ),
    .wr_addr            ( fb_addr ),
    .wr_data            ( fb_wr_data ),
    .wr_be              ( fb_wr_be ),
    .sys_rd_data        ( fb_rd_data ),
    .clk_vid            ( clk_vid ),
    .rd_buf             ( fb_rd_buf ),
    .rd_addr            ( fb_rd_addr ),
    .rd_data            ( vid_fb_data )
);

video_out vo (
    .clk_74a            ( clk_74a ),
    .pal_wr             ( pal_wr ),
    .pal_buf            ( pal_buf ),
    .pal_addr           ( pal_addr ),
    .pal_data           ( pal_data ),
    .swap_req_toggle    ( swap_req_toggle ),
    .swap_ack_toggle    ( swap_ack_toggle ),
    .front              ( front ),
    .frame_toggle       ( frame_toggle ),
    .fb_rd_buf          ( fb_rd_buf ),
    .fb_rd_addr         ( fb_rd_addr ),
    .fb_rd_data         ( vid_fb_data ),
    .clk_vid            ( clk_vid ),
    .reset_n            ( reset_n ),
    .vid_rgb            ( vid_rgb ),
    .vid_de             ( vid_de ),
    .vid_hs             ( vid_hs ),
    .vid_vs             ( vid_vs )
);

    // slot table as the APF leaves it after loading: entry 0 = firmware (id 0),
    // entry 1 = debug log (id 1) with no size, because no debug.txt existed
    reg     [31:0]  dt [0:255];
    reg     [31:0]  dt_r;
    integer i;
    initial begin
        for(i = 0; i < 256; i = i + 1) dt[i] = 32'd0;
        dt[0] = 32'd0;
        dt[1] = 32'd16384;
        dt[2] = 32'd1;
        dt[3] = 32'd0;
    end
always @(posedge clk_74a) begin                 // address and output registered, like mf_datatable
    if(dt_wren) dt[dt_addr[7:0]] <= dt_data;
    dt_r <= dt[dt_addr[7:0]];
    dt_q <= dt_r;
end
    assign log_slot_size = dt[3];

endmodule
