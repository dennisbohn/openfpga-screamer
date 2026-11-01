//
// Simulation top: soc + framebuffer + video_out wired as in core_top (and the I2S output with
// the bit clock, which is no pin there), a model of the
// SDRAM, the SDRAM clock's phase stepper with a PLL that is always
// done, and a model of the core side of the APF data slot table (the table
// core_bridge_cmd keeps)
//

module soc_system (
    input   wire            clk_74a,
    input   wire            clk_vid,
    input   wire            reset_n,

    input   wire            bridge_wr,
    input   wire            bridge_rd,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,
    output  wire    [31:0]  bridge_log_rd_data,

    input   wire    [31:0]  cont1_key,

    output  wire    [23:0]  vid_rgb,
    output  wire            vid_de,
    output  wire            vid_hs,
    output  wire            vid_vs,

    output  wire            audio_sclk,
    output  wire            audio_lrck,
    output  wire            audio_dac,

    output  wire    [31:0]  sim_status,
    output  wire    [31:0]  log_slot_size,      // size word of the debug log's slot table entry (entry 1)
    output  wire    [31:0]  choice_slot_size,   // the same of CHOICE.DAT's (entry 23)
    output  wire            swap_req,           // soc's swap request toggle
    output  wire            front,              // video_out: the buffer in front
    output  wire            sdram_ready,        // what core_top acknowledges data slot writes with
    output  wire    [31:0]  sdram_errors,       // model error count
    output  wire    [31:0]  sdram_max_refresh,  // longest gap between AUTO REFRESH, clocks

    // the core's data slot reads (deferload), answered by the testbench like the APF
    output  wire            ds_read,
    output  wire    [15:0]  ds_id,
    output  wire    [31:0]  ds_offset,
    output  wire    [31:0]  ds_addr,
    output  wire    [31:0]  ds_length,
    input   wire            ds_done,
    input   wire    [2:0]   ds_err
);

    wire            fb_wr_en, fb_buf;
    wire    [13:0]  fb_addr;
    wire    [31:0]  fb_wr_data, fb_rd_data;
    wire    [3:0]   fb_wr_be;
    wire            pal_wr, pal_buf;
    wire    [7:0]   pal_addr;
    wire    [23:0]  pal_data;
    wire            swap_req_toggle, swap_ack_toggle, frame_toggle;
    wire            fb_rd_buf;
    wire    [13:0]  fb_rd_addr;
    wire    [31:0]  vid_fb_data;
    wire    [9:0]   dt_addr;
    wire            dt_wren;
    wire    [31:0]  dt_data;
    reg     [31:0]  dt_q;

    wire    [12:0]  dram_a;
    wire    [1:0]   dram_ba, dram_dqm;
    wire    [15:0]  dram_dq_o, sdm_dq_o;
    wire            dram_dq_oe, sdm_dq_oe;
    wire            dram_cke, dram_ras_n, dram_cas_n, dram_we_n;
    wire            phase_step, phase_later, phase_busy;
    wire    [15:0]  phase_count;
    wire    [4:0]   pll_cntsel;
    wire            pll_updn, pll_phase_en;

    // the data bus: whoever drives; a poison value when nobody does
    wire    [15:0]  dram_dq = sdm_dq_oe ? sdm_dq_o : dram_dq_oe ? dram_dq_o : 16'hD1D1;

soc soc (
    .clk                ( clk_74a ),
    .reset_n            ( reset_n ),
    .sim                ( 1'b1 ),
    .bridge_wr          ( bridge_wr ),
    .bridge_rd          ( bridge_rd ),
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
    .dram_a             ( dram_a ),
    .dram_ba            ( dram_ba ),
    .dram_dq_o          ( dram_dq_o ),
    .dram_dq_oe         ( dram_dq_oe ),
    .dram_dq_i          ( dram_dq ),
    .dram_dqm           ( dram_dqm ),
    .dram_cke           ( dram_cke ),
    .dram_ras_n         ( dram_ras_n ),
    .dram_cas_n         ( dram_cas_n ),
    .dram_we_n          ( dram_we_n ),
    .sdram_pll_locked   ( 1'b1 ),
    .sdram_ready        ( sdram_ready ),
    .phase_step         ( phase_step ),
    .phase_later        ( phase_later ),
    .phase_busy         ( phase_busy ),
    .phase_count        ( phase_count ),
    .ds_read            ( ds_read ),
    .ds_id              ( ds_id ),
    .ds_offset          ( ds_offset ),
    .ds_addr            ( ds_addr ),
    .ds_length          ( ds_length ),
    .ds_done            ( ds_done ),
    .ds_err             ( ds_err ),
    .audio_mclk         ( ),
    .audio_sclk         ( audio_sclk ),
    .audio_lrck         ( audio_lrck ),
    .audio_dac          ( audio_dac ),
    .sim_status         ( sim_status ),
    .opt_music          ( )
);

// no PLL in the simulation: every phase step ends by pll_dps's timeout
pll_dps pll_sd_dps (
    .clk                ( clk_74a ),
    .step               ( phase_step ),
    .later              ( phase_later ),
    .busy               ( phase_busy ),
    .count              ( phase_count ),
    .cntsel             ( pll_cntsel ),
    .updn               ( pll_updn ),
    .phase_en           ( pll_phase_en ),
    .phase_done         ( 1'b1 )
);

sdram_model sdm (
    .clk                ( clk_74a ),
    .cke                ( dram_cke ),
    .ras_n              ( dram_ras_n ),
    .cas_n              ( dram_cas_n ),
    .we_n               ( dram_we_n ),
    .ba                 ( dram_ba ),
    .a                  ( dram_a ),
    .dqm                ( dram_dqm ),
    .dq_i               ( dram_dq_o ),
    .dq_i_en            ( dram_dq_oe ),
    .dq_o               ( sdm_dq_o ),
    .dq_o_en            ( sdm_dq_oe ),
    .error              ( ),
    .error_count        ( sdram_errors ),
    .init_done          ( ),
    .max_refresh_interval ( sdram_max_refresh ),
    .refresh_count      ( )
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

    // the slot table as core_bridge_cmd keeps it: the APF writes it over the bridge
    // (0xF8002000, per slot its id, then its file's size; the testbench does that as the
    // APF does when it loads the core), the soc reads and writes it on its own port
    reg     [31:0]  dt [0:255];
    reg     [31:0]  dt_r;
    integer i;
    initial for(i = 0; i < 256; i = i + 1) dt[i] = 32'd0;
always @(posedge clk_74a) begin                 // address and output registered, like mf_datatable
    if(dt_wren) dt[dt_addr[7:0]] <= dt_data;
    else if(bridge_wr && bridge_addr[31:24] == 8'hF8 && bridge_addr[15:12] == 4'h2)
        dt[bridge_addr[9:2]] <= bridge_wr_data;
    dt_r <= dt[dt_addr[7:0]];
    dt_q <= dt_r;
end
    assign log_slot_size = dt[3];
    assign choice_slot_size = dt[47];
    assign swap_req = swap_req_toggle;

endmodule
