//
// Simulation top for the SDRAM random test: sdram_port + sdram + the SDRAM model, with
// the CPU-side ports, the bridge and the job and pin signals brought out for the
// testbench's bus models and checks (docs/speicher.md)
//

module sdram_system (
    input   wire            clk,
    input   wire            cpu_reset,
    input   wire            reinit,             // initialise again, as after a phase step

    input   wire            ib_cmd_valid,
    output  wire            ib_cmd_ready,
    input   wire    [25:5]  ib_cmd_addr,
    output  wire            ib_rsp_valid,
    output  wire    [31:0]  ib_rsp_data,
    output  wire            ib_rsp_error,

    input   wire            db_cmd_valid,
    output  wire            db_cmd_ready,
    input   wire            db_cmd_wr,
    input   wire            db_cmd_line,
    input   wire    [25:2]  db_cmd_addr,
    input   wire    [31:0]  db_cmd_data,
    input   wire    [3:0]   db_cmd_mask,
    output  wire            db_rsp_valid,
    output  wire    [31:0]  db_rsp_data,
    output  wire            db_rsp_error,
    output  wire            db_rsp_last,

    input   wire            br_wr,
    input   wire    [25:2]  br_addr,
    input   wire    [31:0]  br_data,

    output  wire    [23:0]  br_words,
    output  wire            br_overflow,
    output  wire            br_dropped,
    output  wire            cpu_writes_idle,
    output  wire            sdram_ready,

    // jobs as the controller takes them, and the SDRAM pins
    output  wire            job_take,
    output  wire            job_write,
    output  wire            job_line,
    output  wire    [25:2]  job_addr,
    output  wire    [3:0]   job_mask,
    output  wire    [2:0]   job_tag,
    output  wire    [12:0]  dram_a,
    output  wire    [1:0]   dram_ba,
    output  wire            dram_ras_n,
    output  wire            dram_cas_n,
    output  wire            dram_we_n,

    output  wire    [31:0]  model_errors,
    output  wire    [31:0]  max_refresh
);

    wire            job_valid, job_ready;
    wire    [31:0]  job_wdata;
    wire            rd_valid, rd_last, rd_error, sdram_writing, locked_nc;
    wire    [31:0]  rd_data, refresh_nc;
    wire    [2:0]   rd_tag;
    wire    [15:0]  dram_dq_o, sdm_dq_o;
    wire            dram_dq_oe, sdm_dq_oe, dram_cke;
    wire    [1:0]   dram_dqm;
    wire    [15:0]  dram_dq = sdm_dq_oe ? sdm_dq_o : dram_dq_oe ? dram_dq_o : 16'hD1D1;

    assign job_take = job_valid && job_ready;

sdram_port sdp (
    .clk                ( clk ),
    .cpu_reset          ( cpu_reset ),
    .ib_cmd_valid       ( ib_cmd_valid ),
    .ib_cmd_ready       ( ib_cmd_ready ),
    .ib_cmd_addr        ( ib_cmd_addr ),
    .ib_rsp_valid       ( ib_rsp_valid ),
    .ib_rsp_data        ( ib_rsp_data ),
    .ib_rsp_error       ( ib_rsp_error ),
    .db_cmd_valid       ( db_cmd_valid ),
    .db_cmd_ready       ( db_cmd_ready ),
    .db_cmd_wr          ( db_cmd_wr ),
    .db_cmd_line        ( db_cmd_line ),
    .db_cmd_addr        ( db_cmd_addr ),
    .db_cmd_data        ( db_cmd_data ),
    .db_cmd_mask        ( db_cmd_mask ),
    .db_rsp_valid       ( db_rsp_valid ),
    .db_rsp_data        ( db_rsp_data ),
    .db_rsp_error       ( db_rsp_error ),
    .db_rsp_last        ( db_rsp_last ),
    .br_wr              ( br_wr ),
    .br_addr            ( br_addr ),
    .br_data            ( br_data ),
    .br_words           ( br_words ),
    .br_overflow        ( br_overflow ),
    .br_dropped         ( br_dropped ),
    .cpu_writes_idle    ( cpu_writes_idle ),
    .job_valid          ( job_valid ),
    .job_ready          ( job_ready ),
    .job_write          ( job_write ),
    .job_line           ( job_line ),
    .job_addr           ( job_addr ),
    .job_wdata          ( job_wdata ),
    .job_mask           ( job_mask ),
    .job_tag            ( job_tag ),
    .rd_valid           ( rd_valid ),
    .rd_data            ( rd_data ),
    .rd_tag             ( rd_tag ),
    .rd_last            ( rd_last ),
    .rd_error           ( rd_error ),
    .sdram_ready        ( sdram_ready ),
    .sdram_writing      ( sdram_writing )
);

sdram sdc (
    .clk                ( clk ),
    .pll_locked         ( 1'b1 ),
    .reinit             ( reinit ),
    .job_valid          ( job_valid ),
    .job_ready          ( job_ready ),
    .job_write          ( job_write ),
    .job_line           ( job_line ),
    .job_addr           ( job_addr ),
    .job_wdata          ( job_wdata ),
    .job_mask           ( job_mask ),
    .job_tag            ( job_tag ),
    .rd_valid           ( rd_valid ),
    .rd_data            ( rd_data ),
    .rd_tag             ( rd_tag ),
    .rd_last            ( rd_last ),
    .rd_error           ( rd_error ),
    .ready              ( sdram_ready ),
    .locked             ( locked_nc ),
    .writing            ( sdram_writing ),
    .refresh_count      ( refresh_nc ),
    .dram_a             ( dram_a ),
    .dram_ba            ( dram_ba ),
    .dram_dq_o          ( dram_dq_o ),
    .dram_dq_oe         ( dram_dq_oe ),
    .dram_dq_i          ( dram_dq ),
    .dram_dqm           ( dram_dqm ),
    .dram_cke           ( dram_cke ),
    .dram_ras_n         ( dram_ras_n ),
    .dram_cas_n         ( dram_cas_n ),
    .dram_we_n          ( dram_we_n )
);

sdram_model sdm (
    .clk                ( clk ),
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
    .error_count        ( model_errors ),
    .init_done          ( ),
    .max_refresh_interval ( max_refresh ),
    .refresh_count      ( )
);

endmodule
