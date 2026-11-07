//
// soc: the CPU system and everything the CPU talks to (docs/soc.md, docs/memory.md).
//
// Contents: the VexRiscv CPU, the 32 KB boot RAM, the 4 KB debug log, the system and video
// registers, the framebuffer drawing port, the SDRAM controller with its front end
// (sdram_port.v, sdram.v), the 3D units (geom.v, setup.v, raster.v), the blitter (blit.v),
// the sound mixer (audio.v), the FM chip (opl.v) and the CFU (cfu_fixmul.v). The Pocket's
// SRAM and PSRAM are not used.
//
// Clock and reset: everything runs on clk (clk_74a). The CPU is held in reset while
// reset_n is low. The APF loads the firmware data slot into the boot RAM before it
// releases reset. The SDRAM controller is not reset with the CPU.
//
// Byte order: the bridge delivers big-endian words and the CPU is little-endian. Bridge
// data into and out of the CPU's memories is therefore byte swapped (swap32). The
// framebuffer is swapped too, because its words keep the leftmost pixel in bits 31:24.
//
// Deferred slots: the SDRAM data slots are deferload. Once the SDRAM clock phase is found,
// the CPU asks the APF for them through the ds_* ports, because the phase sweep may write
// anywhere in the SDRAM at a bad phase.
//
// Bridge reads: the APF expects the answer to a read one read later (io_bridge_peripheral.v:
// "reads are buffered by 1 word"). The data for the address of a read is latched at its
// bridge_rd pulse and sent in the next read transaction. The APF reads one word more than it
// needs and drops the first answer. core_bridge_cmd answers the same way.
//
// Buses: VexRiscv's native cache buses.
//   iBus  One command per 32-byte line, answered with 8 words in order. The boot RAM is
//         served here, SDRAM lines through sdram_port, every other line with 8 words of
//         which the first seven carry the error flag.
//   dBus  Line reads (size 5, 8 words), single reads (1 word) and single writes (with a
//         byte mask, no answer). A read blocks the next command until its last word is
//         out. Writes are posted. cmd_ready comes from flops and stays low until every
//         target could take the next command, whichever it is.
//

module soc (
    input   wire            clk,
    input   wire            reset_n,
    input   wire            sim,                    // 1 only in the simulation top

    // APF bridge, synchronous to clk. It carries the firmware into the boot RAM, the data
    // slots into the SDRAM and the debug log out.
    input   wire            bridge_wr,
    input   wire            bridge_rd,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,
    output  reg     [31:0]  bridge_log_rd_data,     // read data for the bridge addresses of the
                                                    // debug log (0x0010_0xxx) and of the SDRAM

    // controller 1, synchronous to clk
    input   wire    [31:0]  cont1_key,
    input   wire    [31:0]  cont1_joy,
    input   wire    [15:0]  cont1_trig,

    // framebuffer drawing port (framebuffer.v), read data one clock after the address
    output  wire            fb_wr_en,
    output  wire            fb_buf,
    output  wire    [13:0]  fb_addr,
    output  wire    [31:0]  fb_wr_data,
    output  wire    [3:0]   fb_wr_be,
    input   wire    [31:0]  fb_rd_data,

    // video_out: palette write port of the back buffer (pal_wr pulses for one clock), swap
    // request and acknowledge as toggles, and the frame tick as a toggle
    output  reg             pal_wr = 0,
    output  reg             pal_buf,
    output  reg     [7:0]   pal_addr,
    output  reg     [23:0]  pal_data,
    output  reg             swap_req_toggle = 0,
    input   wire            swap_ack_toggle,        // clk_vid domain
    input   wire            frame_toggle,           // clk_vid domain

    // core side of the APF data slot table (core_bridge_cmd). The read data comes 2 clocks
    // after the address. Per slot, word 0 is its id and word 1 the size of its file.
    output  reg     [9:0]   dt_addr,
    output  reg             dt_wren = 0,
    output  reg     [31:0]  dt_data,
    input   wire    [31:0]  dt_q,

    // SDRAM pins (the data bus is split; the tristate is in core_top) and its PLL
    output  wire    [12:0]  dram_a,
    output  wire    [1:0]   dram_ba,
    output  wire    [15:0]  dram_dq_o,
    output  wire            dram_dq_oe,
    input   wire    [15:0]  dram_dq_i,
    output  wire    [1:0]   dram_dqm,
    output  wire            dram_cke,
    output  wire            dram_ras_n,
    output  wire            dram_cas_n,
    output  wire            dram_we_n,
    input   wire            sdram_pll_locked,       // asynchronous
    output  wire            sdram_ready,
    output  reg             phase_step = 0,         // one clock: move the SDRAM clock phase by a step
    output  reg             phase_later,            // direction of the step: 1 later, 0 earlier
    input   wire            phase_busy,             // a step is in progress
    input   wire    [15:0]  phase_count,            // steps from the start phase, signed

    // A data slot read that the core asks the APF for (core_bridge_cmd's target command
    // 0x0180; used for the deferload slots). ds_read is high from the start until ds_done.
    output  reg             ds_read = 0,
    output  reg     [15:0]  ds_id,
    output  reg     [31:0]  ds_offset,
    output  reg     [31:0]  ds_addr,
    output  reg     [31:0]  ds_length,
    input   wire            ds_done,
    input   wire    [2:0]   ds_err,

    // I2S to the APF (audio.v). The bit clock is no pin; only the simulation reads it.
    output  wire            audio_mclk,
    output  wire            audio_sclk,
    output  wire            audio_lrck,
    output  wire            audio_dac,

    output  reg     [31:0]  sim_status = 0,         // SYS_SIM, for the testbench
    output  wire    [31:0]  opt_music               // the Pocket menu's Music Source (bridge 0x2000_0000)
);

    localparam  [31:0]  SYS_ID = 32'h53435231;      // "SCR1"
    localparam  [15:0]  LOG_SLOT_ID = 16'd1;        // data slot of debug.txt (core/data.json)
    localparam  [31:0]  LOG_SIZE = 32'd4096;

    // reverses the byte order of a word (bridge big-endian <-> CPU little-endian)
    function [31:0] swap32(input [31:0] d);
        swap32 = {d[7:0], d[15:8], d[23:16], d[31:24]};
    endfunction

    // reset of the CPU and of everything in front of its buses: reset_n, registered
    reg         cpu_reset = 1;
always @(posedge clk) cpu_reset <= ~reset_n;

//
// the CPU: VexRiscv with I-cache and D-cache (VexPocketIMFC, generated by tools/vexriscv/gen.sh)
// and the CFU port, which cfu_fixmul answers
//
    wire        ibus_cmd_valid;
    wire        ibus_cmd_ready;
    wire [31:0] ibus_cmd_addr;
    wire        ibus_rsp_valid;
    wire [31:0] ibus_rsp_data;
    wire        ibus_rsp_error;

    wire        dbus_cmd_valid;
    wire        dbus_cmd_ready;
    wire        dbus_cmd_wr;
    wire [31:0] dbus_cmd_addr;
    wire [31:0] dbus_cmd_data;
    wire [3:0]  dbus_cmd_mask;
    wire [2:0]  dbus_cmd_size;
    wire        dbus_rsp_valid;
    wire        dbus_rsp_last;
    wire [31:0] dbus_rsp_data;
    wire        dbus_rsp_error;

    wire [2:0]  ibus_cmd_size_nc;            // always a 32-byte line
    wire        dbus_cmd_uncached_nc, dbus_cmd_last_nc;
    wire        cfu_cmd_valid, cfu_cmd_ready, cfu_rsp_valid, cfu_rsp_ready;
    wire [9:0]  cfu_function_id;
    wire [31:0] cfu_in0, cfu_in1, cfu_out;

VexPocketIMFC cpu (
    .clk                                    ( clk ),
    .reset                                  ( cpu_reset ),
    .timerInterrupt                         ( 1'b0 ),
    .externalInterrupt                      ( 1'b0 ),
    .softwareInterrupt                      ( 1'b0 ),

    .iBus_cmd_valid                         ( ibus_cmd_valid ),
    .iBus_cmd_ready                         ( ibus_cmd_ready ),
    .iBus_cmd_payload_address               ( ibus_cmd_addr ),
    .iBus_cmd_payload_size                  ( ibus_cmd_size_nc ),
    .iBus_rsp_valid                         ( ibus_rsp_valid ),
    .iBus_rsp_payload_data                  ( ibus_rsp_data ),
    .iBus_rsp_payload_error                 ( ibus_rsp_error ),

    .dBus_cmd_valid                         ( dbus_cmd_valid ),
    .dBus_cmd_ready                         ( dbus_cmd_ready ),
    .dBus_cmd_payload_wr                    ( dbus_cmd_wr ),
    .dBus_cmd_payload_uncached              ( dbus_cmd_uncached_nc ),
    .dBus_cmd_payload_address               ( dbus_cmd_addr ),
    .dBus_cmd_payload_data                  ( dbus_cmd_data ),
    .dBus_cmd_payload_mask                  ( dbus_cmd_mask ),
    .dBus_cmd_payload_size                  ( dbus_cmd_size ),
    .dBus_cmd_payload_last                  ( dbus_cmd_last_nc ),
    .dBus_rsp_valid                         ( dbus_rsp_valid ),
    .dBus_rsp_payload_last                  ( dbus_rsp_last ),
    .dBus_rsp_payload_data                  ( dbus_rsp_data ),
    .dBus_rsp_payload_error                 ( dbus_rsp_error ),

    .CfuPlugin_bus_cmd_valid                ( cfu_cmd_valid ),
    .CfuPlugin_bus_cmd_ready                ( cfu_cmd_ready ),
    .CfuPlugin_bus_cmd_payload_function_id  ( cfu_function_id ),
    .CfuPlugin_bus_cmd_payload_inputs_0     ( cfu_in0 ),
    .CfuPlugin_bus_cmd_payload_inputs_1     ( cfu_in1 ),
    .CfuPlugin_bus_rsp_valid                ( cfu_rsp_valid ),
    .CfuPlugin_bus_rsp_ready                ( cfu_rsp_ready ),
    .CfuPlugin_bus_rsp_payload_outputs_0    ( cfu_out )
);

cfu_fixmul cfu (
    .clk                    ( clk ),
    .reset                  ( cpu_reset ),
    .cmd_valid              ( cfu_cmd_valid ),
    .cmd_ready              ( cfu_cmd_ready ),
    .cmd_payload_function_id( cfu_function_id ),
    .cmd_payload_inputs_0   ( cfu_in0 ),
    .cmd_payload_inputs_1   ( cfu_in1 ),
    .rsp_valid              ( cfu_rsp_valid ),
    .rsp_ready              ( cfu_rsp_ready ),
    .rsp_payload_outputs_0  ( cfu_out )
);

//
// boot RAM: 8192 words (32 KB) in four byte lanes. Port A serves the iBus, port B the dBus,
// or the bridge while the CPU is held in reset (the firmware load).
//
    reg  [12:0] boot_addr_a;
    wire [31:0] boot_q_a, boot_q_b;
    reg  [12:0] boot_addr_b;
    reg  [3:0]  boot_we_b;
    reg  [31:0] boot_wdata_b;

    genvar lane;
    generate
        for(lane = 0; lane < 4; lane = lane + 1) begin : boot_lanes
            ram_lane #(.ADDR_W(13)) ram (
                .clk_a      ( clk ),
                .addr_a     ( boot_addr_a ),
                .we_a       ( 1'b0 ),
                .wdata_a    ( 8'h00 ),
                .q_a        ( boot_q_a[lane*8 +: 8] ),
                .clk_b      ( clk ),
                .addr_b     ( boot_addr_b ),
                .we_b       ( boot_we_b[lane] ),
                .wdata_b    ( boot_wdata_b[lane*8 +: 8] ),
                .q_b        ( boot_q_b[lane*8 +: 8] )
            );
        end
    endgenerate

//
// debug log: 1024 words (4 KB), port A for the dBus, port B for the bridge. Port B only
// reads. The APF loads the old debug.txt (or 0xFF) into the slot at boot; ignoring that
// keeps the log as it was until the firmware clears it. The log is zero after the
// bitstream is loaded, so a run in which the CPU never starts saves zeros, not the old
// file. A firmware reload from the menu is only a reset and leaves the previous run's log.
//
    reg  [9:0]  log_addr_a;
    reg  [3:0]  log_we_a;
    reg  [31:0] log_wdata_a;
    wire [31:0] log_q_a, log_q_b;
    wire        bridge_log = bridge_addr[31:12] == 20'h00100;

    generate
        for(lane = 0; lane < 4; lane = lane + 1) begin : log_lanes
            ram_lane #(.ADDR_W(10)) ram (
                .clk_a      ( clk ),
                .addr_a     ( log_addr_a ),
                .we_a       ( log_we_a[lane] ),
                .wdata_a    ( log_wdata_a[lane*8 +: 8] ),
                .q_a        ( log_q_a[lane*8 +: 8] ),
                .clk_b      ( clk ),
                .addr_b     ( bridge_addr[11:2] ),
                .we_b       ( 1'b0 ),
                .wdata_b    ( 8'h00 ),
                .q_b        ( log_q_b[lane*8 +: 8] )
            );
        end
    endgenerate

    // The answer for the next read transaction (see the header). It comes from the debug log
    // or, when the APF saves the nonvolatile slots (CHOICE.DAT and ATTACK.DAT) from the
    // SDRAM, from the SDRAM as soon as sdram_port has the word.
    wire        br_rd_valid;
    wire [31:0] br_rd_data;
    // holds the word that the next bridge read transaction returns
always @(posedge clk) begin
    if(bridge_rd && bridge_log) bridge_log_rd_data <= swap32(log_q_b);
    else if(br_rd_valid) bridge_log_rd_data <= swap32(br_rd_data);
end

//
// SDRAM: controller (sdram.v) and front end with the arbiter (sdram_port.v), docs/memory.md.
// The signals named sdp_ib_* and sdp_db_* belong to the CPU's iBus and dBus ports of
// sdram_port, job_* and rd_* to the controller's job port, and br_* to the bridge.
//
    wire        sdp_ib_cmd_valid, sdp_ib_cmd_ready;
    wire        sdp_ib_rsp_valid, sdp_ib_rsp_error;
    wire [31:0] sdp_ib_rsp_data;
    wire        sdp_db_cmd_valid, sdp_db_cmd_ready;
    wire        sdp_db_rsp_valid, sdp_db_rsp_error, sdp_db_rsp_last;
    wire [31:0] sdp_db_rsp_data;
    wire [23:0] br_words;
    wire        br_overflow, br_dropped, sdram_cpu_writes_idle;
    wire        job_valid, job_ready, job_write, job_line;
    wire [25:2] job_addr;
    wire [31:0] job_wdata;
    wire [3:0]  job_mask;
    wire [2:0]  job_tag;
    wire        rd_valid, rd_last, rd_error;
    wire [31:0] rd_data;
    wire [2:0]  rd_tag;
    wire        sdram_locked, sdram_writing;
    wire [31:0] sdram_refreshes;
    reg         sdram_reinit = 0;

    wire        bridge_sdram = bridge_addr[31:26] == 6'b000100;
    wire        rs_mem_valid, rs_mem_ready, rs_mem_rsp_valid;     // texture lines (raster.v)
    wire [25:5] rs_mem_addr;
    wire [31:0] rs_mem_rsp_data;
    wire        gu_mem_valid, gu_mem_ready, gu_mem_rsp_valid;     // mesh lines (geom.v)
    wire [25:5] gu_mem_addr;
    wire        bl_mem_valid, bl_mem_ready, bl_mem_rsp_valid;     // picture lines (blit.v)
    wire [25:5] bl_mem_addr;
    wire        au_mem_valid, au_mem_ready, au_mem_rsp_valid;     // sample lines (audio.v)
    wire [25:5] au_mem_addr;

    // The mixer, the rasterizer, the geometry unit and the blitter share sdram_port's line
    // port (named rs_* there), one line request at a time. The priority is:
    //   1. the mixer (it needs a line only now and then, but by the next output value)
    //   2. the rasterizer (its pipeline waits for the line)
    //   3. the geometry unit
    //   4. the blitter
    wire        lp_valid = au_mem_valid || rs_mem_valid || gu_mem_valid || bl_mem_valid,
                lp_ready, lp_rsp_valid, lp_rsp_last;
    // Who owns the lines in flight: 0 rasterizer, 1 geometry unit, 2 blitter, 3 mixer. There
    // are up to two lines in flight (lp_n). lp_own0 is the older one: the words come back in
    // the order in which the lines were asked for.
    reg  [1:0]  lp_own0 = 0, lp_own1 = 0;
    reg  [1:0]  lp_n = 0;
    wire [1:0]  lp_owner = lp_own0;
    wire [1:0]  lp_new = au_mem_valid ? 2'd3 : rs_mem_valid ? 2'd0 : gu_mem_valid ? 2'd1 : 2'd2;
    wire        lp_push = lp_ready && lp_valid, lp_pop = lp_rsp_valid && lp_rsp_last;
    assign au_mem_ready = lp_ready && au_mem_valid;
    assign rs_mem_ready = lp_ready && !au_mem_valid && rs_mem_valid;
    assign gu_mem_ready = lp_ready && !au_mem_valid && !rs_mem_valid && gu_mem_valid;
    assign bl_mem_ready = lp_ready && !au_mem_valid && !rs_mem_valid && !gu_mem_valid && bl_mem_valid;
    assign rs_mem_rsp_valid = lp_rsp_valid && lp_owner == 2'd0;
    assign gu_mem_rsp_valid = lp_rsp_valid && lp_owner == 2'd1;
    assign bl_mem_rsp_valid = lp_rsp_valid && lp_owner == 2'd2;
    assign au_mem_rsp_valid = lp_rsp_valid && lp_owner == 2'd3;
    // two-entry queue of line owners: push at a request, pop with the last word of a line
always @(posedge clk) begin
    if(lp_push && !lp_pop) begin
        if(lp_n == 2'd0) lp_own0 <= lp_new;
        else lp_own1 <= lp_new;
        lp_n <= lp_n + 1'b1;
    end else if(lp_pop && !lp_push) begin
        lp_own0 <= lp_own1;
        lp_n <= lp_n - 1'b1;
    end else if(lp_pop && lp_push) begin
        if(lp_n == 2'd1) lp_own0 <= lp_new;
        else begin
            lp_own0 <= lp_own1;
            lp_own1 <= lp_new;
        end
    end
    if(cpu_reset) lp_n <= 0;
end
    // RS_STARVED: counts the clocks in which the rasterizer is idle while the setup unit or
    // the geometry unit is busy. Cleared by a write to RS_CLOCKS (0xF000_3054), which clears
    // the other rasterizer counters too.
always @(posedge clk)
    if(rs_wr && d_addr[6:2] == 5'd21) rs_starved <= 0;
    else if(!rs_busy && (su_busy || gu_busy)) rs_starved <= rs_starved + 1'b1;

sdram_port sdp (
    .clk                ( clk ),
    .cpu_reset          ( cpu_reset ),
    .ib_cmd_valid       ( sdp_ib_cmd_valid ),
    .ib_cmd_ready       ( sdp_ib_cmd_ready ),
    .ib_cmd_addr        ( ibus_cmd_addr[25:5] ),
    .ib_rsp_valid       ( sdp_ib_rsp_valid ),
    .ib_rsp_data        ( sdp_ib_rsp_data ),
    .ib_rsp_error       ( sdp_ib_rsp_error ),
    .db_cmd_valid       ( sdp_db_cmd_valid ),
    .db_cmd_ready       ( sdp_db_cmd_ready ),
    .db_cmd_wr          ( dbus_cmd_wr ),
    .db_cmd_line        ( dbus_cmd_size == 3'd5 ),
    .db_cmd_addr        ( dbus_cmd_addr[25:2] ),
    .db_cmd_data        ( dbus_cmd_data ),
    .db_cmd_mask        ( dbus_cmd_mask ),
    .db_rsp_valid       ( sdp_db_rsp_valid ),
    .db_rsp_data        ( sdp_db_rsp_data ),
    .db_rsp_error       ( sdp_db_rsp_error ),
    .db_rsp_last        ( sdp_db_rsp_last ),
    .rs_cmd_valid       ( lp_valid ),
    .rs_cmd_ready       ( lp_ready ),
    .rs_cmd_addr        ( au_mem_valid ? au_mem_addr :
                          rs_mem_valid ? rs_mem_addr :
                          gu_mem_valid ? gu_mem_addr : bl_mem_addr ),
    .rs_rsp_valid       ( lp_rsp_valid ),
    .rs_rsp_last        ( lp_rsp_last ),
    .rs_rsp_data        ( rs_mem_rsp_data ),
    .br_wr              ( bridge_wr && bridge_sdram ),
    .br_addr            ( bridge_addr[25:2] ),
    .br_data            ( swap32(bridge_wr_data) ),
    .br_words           ( br_words ),
    .br_overflow        ( br_overflow ),
    .br_dropped         ( br_dropped ),
    .br_rd              ( bridge_rd && bridge_sdram ),
    .br_rd_addr         ( bridge_addr[25:2] ),
    .br_rd_valid        ( br_rd_valid ),
    .br_rd_data         ( br_rd_data ),
    .cpu_writes_idle    ( sdram_cpu_writes_idle ),
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
    .pll_locked         ( sdram_pll_locked ),
    .reinit             ( sdram_reinit ),
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
    .locked             ( sdram_locked ),
    .writing            ( sdram_writing ),
    .refresh_count      ( sdram_refreshes ),
    .dram_a             ( dram_a ),
    .dram_ba            ( dram_ba ),
    .dram_dq_o          ( dram_dq_o ),
    .dram_dq_oe         ( dram_dq_oe ),
    .dram_dq_i          ( dram_dq_i ),
    .dram_dqm           ( dram_dqm ),
    .dram_cke           ( dram_cke ),
    .dram_ras_n         ( dram_ras_n ),
    .dram_cas_n         ( dram_cas_n ),
    .dram_we_n          ( dram_we_n )
);

//
// iBus: boot RAM lines are served here at one word per clock, SDRAM lines through
// sdram_port. Any other line is answered with the error flag on its first seven words but
// not on the eighth. VexRiscv's InstructionCache sets its hadError after clearing it on
// the completing word, so an error on the eighth word would stick and mark the next line
// bad as well, the trap handler's included. The first seven words alone mark the line bad.
//
    reg         ib_busy = 0;                // a boot RAM line is being read out
    reg         ib_ok;                      // the line is inside the boot RAM
    reg  [9:0]  ibus_line;                  // the line's address, bits 14:5
    reg  [2:0]  ib_cnt;                     // the word being addressed
    reg         ib_issued = 0;              // a word was addressed last clock
    reg         ib_issued_ok;               // ib_ok, delayed with the word
    reg         ib_issued_last;             // the word is the eighth
    reg         ib_rsp_valid = 0;
    reg  [31:0] ib_rsp_data;
    reg         ib_rsp_error;

    wire        ib_sdram = ibus_cmd_addr[31:26] == 6'b000100;

    assign ibus_cmd_ready = !ib_busy && sdp_ib_cmd_ready && !cpu_reset;
    assign sdp_ib_cmd_valid = ibus_cmd_valid && ibus_cmd_ready && ib_sdram;

    // The two sources never answer at the same time: only one line is in flight.
    assign ibus_rsp_valid = ib_rsp_valid || sdp_ib_rsp_valid;
    assign ibus_rsp_data = sdp_ib_rsp_valid ? sdp_ib_rsp_data : ib_rsp_data;
    assign ibus_rsp_error = sdp_ib_rsp_valid ? sdp_ib_rsp_error : ib_rsp_error;

    // boot RAM port A address: the line's word counter while busy, else the command's address
always @(*) begin
    boot_addr_a = ib_busy ? {ibus_line, ib_cnt} : ibus_cmd_addr[14:2];
end

    // Boot RAM and error lines. On a command (not SDRAM) the line is accepted into ib_busy;
    // then one word is addressed per clock for 8 clocks, and the RAM's data and the error
    // flag follow one clock later as the response.
always @(posedge clk) begin
    ib_rsp_valid <= ib_issued;
    ib_rsp_data <= boot_q_a;
    ib_rsp_error <= !ib_issued_ok && !ib_issued_last;
    ib_issued <= 0;
    if(cpu_reset) begin
        ib_busy <= 0;
    end else if(!ib_busy) begin
        if(ibus_cmd_valid && ibus_cmd_ready && !ib_sdram) begin
            ib_busy <= 1;
            ib_ok <= ibus_cmd_addr[31:15] == 0;
            ibus_line <= ibus_cmd_addr[14:5];
            ib_cnt <= 0;
        end
    end else begin
        ib_issued <= 1;
        ib_issued_ok <= ib_ok;
        ib_issued_last <= ib_cnt == 3'd7;
        ib_cnt <= ib_cnt + 1'b1;
        if(ib_cnt == 3'd7) ib_busy <= 0;
    end
end

//
// dBus: a command is decoded into a region, remembered in d_* and carried out in the clock
// after it. The state machine d_state follows reads; writes are posted and need no state.
//
    // address regions (docs/soc.md, CPU memory map)
    localparam  R_NONE  = 3'd0;             // no target: reads answer with the error flag
    localparam  R_BOOT  = 3'd1;             // boot RAM, 0x0000_0000
    localparam  R_LOG   = 3'd2;             // debug log, 0xF000_8000
    localparam  R_FB    = 3'd3;             // framebuffers, 0xF100_0000 and 0xF101_0000
    localparam  R_REG   = 3'd4;             // registers, 0xF000_0000 to 0xF000_5FFF
    localparam  R_SDRAM = 3'd5;             // cached window and the uncached one

    // region of a CPU address
    function [2:0] region(input [31:0] a);
        if(a[31:15] == 0)                                   region = R_BOOT;
        else if(a[31:26] == 6'b000100 || a[31:26] == 6'b111101) region = R_SDRAM;
        else if(a[31:12] == 20'hF0008)                      region = R_LOG;
        else if(a[31:17] == 15'h7880 && a[15:2] < 14'd16000) region = R_FB;
        else if(a[31:12] == 20'hF0000 || a[31:12] == 20'hF0001 || a[31:12] == 20'hF0002 ||
                a[31:12] == 20'hF0003 || a[31:12] == 20'hF0004 || a[31:12] == 20'hF0005) region = R_REG;
        else                                                region = R_NONE;
    endfunction

    // states of a read
    localparam  D_IDLE   = 2'd0;            // ready for the next command
    localparam  D_SINGLE = 2'd1;            // fixed-latency single read (2 clocks)
    localparam  D_LINE   = 2'd2;            // line read from the boot RAM (or error words)
    localparam  D_EXT    = 2'd3;            // read from the SDRAM, answered by sdram_port

    reg  [1:0]  d_state = D_IDLE;
    reg  [31:0] d_addr;                     // the command being worked on
    reg  [31:0] d_data;
    reg  [3:0]  d_mask;
    reg         d_wr;
    reg  [2:0]  d_region;
    reg         d_write = 0;                // a posted write to do this clock
    reg  [2:0]  d_cnt;                      // line read: the word being addressed
    reg         d_phase;                    // single read: 0 = address out, 1 = data in
    reg         d_issued = 0;               // line read: a word was addressed last clock
    reg         d_rsp_valid = 0;
    reg         d_rsp_last;
    reg  [31:0] d_rsp_data;
    reg         d_rsp_error;

    wire [2:0]  cmd_region = region(dbus_cmd_addr);

//
// The 3D pipeline: geometry unit (geom.v, registers at 0xF000_3100) -> setup unit (setup.v,
// registers at 0xF000_3080) -> rasterizer (raster.v, registers at 0xF000_3000). The
// rasterizer owns the framebuffer port while it draws. The blitter (blit.v, registers at
// 0xF000_3180) uses the port when the three units are idle.
//
// The CPU's accesses wait (dbus_cmd_ready stays low, see rs_waits) in these cases:
//   - A framebuffer access or a draw command (RS_DRAW) waits until all units are done.
//   - A corner for the setup unit (SU_T_NEXT, SU_T_FIRST) waits until the unit can take it
//     and the geometry unit is done, because both fill its input.
//   - A word of a piece for the geometry unit (GU_PUSH) waits while its piece buffer is full.
//   - A command word for the blitter (BL_PUSH) waits while its command buffer is full.
//
    wire        rs_busy, rs_fb_wr, rs_fb_buf, rs_su_accept;
    wire [31:0] rs_pixels, rs_misses, rs_busy_clocks, rs_fb_data;
    wire [31:0] rs_miss_clocks, rs_edge_clocks, rs_row_clocks, rs_drain_clocks;
    reg  [31:0] rs_starved = 0;                 // clocks in which the rasterizer had nothing to do
                                                // while the units in front of it worked
    wire [13:0] rs_fb_addr;
    wire [3:0]  rs_fb_be;
    wire        su_busy, su_ready, su_go;
    wire [31:0] su_busy_clocks, su_v0, su_v1, su_v2, su_ref, su_tex, su_size;
    wire [31:0] su_a0, su_a1, su_a2, su_dx0, su_dx1, su_dx2, su_dy0, su_dy1, su_dy2;
    wire [9:0]  su_draw;
    // register writes of the four units (the posted write of the clock after the command)
    wire        rs_wr = d_write && d_region == R_REG && d_addr[15:7] == 9'h060;   // 0x3000-0x307C
    wire        rs_start = rs_wr && d_addr[6:2] == 5'd5;                          // RS_DRAW
    wire        su_wr = d_write && d_region == R_REG && d_addr[15:7] == 9'h061;   // 0x3080-0x30FC
    wire        su_corner = su_wr && (d_addr[5:2] == 4'd9 || d_addr[5:2] == 4'd10); // SU_T_NEXT, SU_T_FIRST
    wire        gu_wr = d_write && d_region == R_REG && d_addr[15:7] == 9'h062;   // 0x3100-0x317C
    wire        gu_push = gu_wr && d_addr[6:2] == 5'd23;                          // GU_PUSH
    wire        bl_wr = d_write && d_region == R_REG && d_addr[15:7] == 9'h063;   // 0x3180-0x31FC
    wire        bl_push = bl_wr && d_addr[6:2] == 5'd0;                           // BL_PUSH
    wire        bl_busy, bl_full, bl_fb_wr, bl_fb_buf;
    wire [31:0] bl_busy_clocks, bl_fb_data;
    wire [13:0] bl_fb_addr;
    wire [3:0]  bl_fb_be;
    wire        gu_busy, gu_full, gu_poly_valid, gu_poly_ready;
    wire [31:0] gu_faces, gu_busy_clocks, gu_poly_tex, gu_poly_size;
    wire [2:0]  gu_poly_n;
    wire [9:0]  gu_poly_mode;
    wire [575:0] gu_poly_c;
    // The dBus command waits when it is
    //   (a) a framebuffer access or a write of RS_DRAW (0x3014) while any unit works or has
    //       a posted write for it,
    //   (b) a write of SU_T_NEXT or SU_T_FIRST (0x30A4, 0x30A8) while the setup unit cannot
    //       take a corner or the geometry unit works,
    //   (c) a write of GU_PUSH (0x315C) while the geometry unit's piece buffer is full,
    //   (d) a write of BL_PUSH (0x3180) while the blitter's command buffer is full.
    // A posted write of the previous command (rs_start, su_corner, gu_push, bl_push) counts
    // like a busy unit.
    wire        rs_waits = ((rs_busy || rs_start || su_busy || su_corner || gu_busy || gu_push ||
                             bl_busy || bl_push) &&
                            (cmd_region == R_FB ||
                             (dbus_cmd_wr && cmd_region == R_REG && dbus_cmd_addr[15:0] == 16'h3014))) ||
                           ((!su_ready || su_corner || gu_busy || gu_push) && dbus_cmd_wr &&
                            cmd_region == R_REG &&
                            (dbus_cmd_addr[15:0] == 16'h30A4 || dbus_cmd_addr[15:0] == 16'h30A8)) ||
                           ((gu_full || gu_push) && dbus_cmd_wr && cmd_region == R_REG &&
                            dbus_cmd_addr[15:0] == 16'h315C) ||
                           ((bl_full || bl_push) && dbus_cmd_wr && cmd_region == R_REG &&
                            dbus_cmd_addr[15:0] == 16'h3180);

// The blitter (blit.v), registers at 0xF000_3180. It draws the race screen once the units
// above are done, through the framebuffer port when the rasterizer does not have it.
blit bl (
    .clk                ( clk ),
    .reset              ( cpu_reset ),
    .wr                 ( bl_wr ),
    .wr_addr            ( d_addr[6:2] ),
    .wr_data            ( d_data ),
    .busy               ( bl_busy ),
    .full               ( bl_full ),
    .busy_clocks        ( bl_busy_clocks ),
    .go                 ( !rs_busy && !su_busy && !gu_busy ),
    .fb_wr              ( bl_fb_wr ),
    .fb_buf             ( bl_fb_buf ),
    .fb_addr            ( bl_fb_addr ),
    .fb_data            ( bl_fb_data ),
    .fb_be              ( bl_fb_be ),
    .mem_valid          ( bl_mem_valid ),
    .mem_ready          ( bl_mem_ready ),
    .mem_addr           ( bl_mem_addr ),
    .mem_rsp_valid      ( bl_mem_rsp_valid ),
    .mem_rsp_data       ( rs_mem_rsp_data )
);

// The sound mixer (audio.v): registers at 0xF000_4000, sample lines over the line port and
// the I2S output. The CPU's writes to it never wait.
    wire        au_wr = d_write && d_region == R_REG && d_addr[15:11] == 5'b01000;     // 0x4000-0x47FC
    wire [31:0] au_playing;
    wire        au_late;
    wire [16:0] au_music;
    wire        au_frame;

// The FM music (opl.v): registers at 0xF000_5000. Its output value goes into the mixer's sums.
    wire        fm_wr = d_write && d_region == R_REG && d_addr[15:5] == 11'b0101_0000_000;   // 0x5000-0x501C
    wire signed [15:0] fm_value;
    wire        fm_run, fm_ready, fm_busy;
    wire [8:0]  fm_fill;
    wire [15:0] fm_now;

opl fm_unit (
    .clk                ( clk ),
    .reset              ( cpu_reset ),
    .wr                 ( fm_wr ),
    .wr_addr            ( d_addr[4:2] ),
    .wr_data            ( d_data ),
    .run_out            ( fm_run ),
    .fill               ( fm_fill ),
    .ready              ( fm_ready ),
    .busy               ( fm_busy ),
    .now                ( fm_now ),
    .frame              ( au_frame ),
    .fm                 ( fm_value )
);

// The Pocket menu's Music Source (interact.json). The bridge writes it and the CPU reads it.
// A reset of the CPU does not clear it, because the Pocket writes the saved value just
// before Reset Exit.
    reg  [31:0] opt_music_r = 0;
    // latch the word that the bridge writes to 0x2000_0000
always @(posedge clk) if(bridge_wr && bridge_addr == 32'h2000_0000) opt_music_r <= bridge_wr_data;
    assign opt_music = opt_music_r;

audio au (
    .clk                ( clk ),
    .reset              ( cpu_reset ),
    .wr                 ( au_wr ),
    .wr_addr            ( d_addr[10:2] ),
    .wr_data            ( d_data ),
    .playing            ( au_playing ),
    .late               ( au_late ),
    .busy               ( ),
    .music              ( au_music ),
    .frame              ( au_frame ),
    .fm                 ( fm_value ),
    .mem_valid          ( au_mem_valid ),
    .mem_ready          ( au_mem_ready ),
    .mem_addr           ( au_mem_addr ),
    .mem_rsp_valid      ( au_mem_rsp_valid ),
    .mem_rsp_data       ( rs_mem_rsp_data ),
    .i2s_mclk           ( audio_mclk ),
    .i2s_sclk           ( audio_sclk ),
    .i2s_lrck           ( audio_lrck ),
    .i2s_dac            ( audio_dac )
);

// The geometry unit (geom.v): registers at 0xF000_3100. It reads meshes over the line port
// and hands finished polygons to the setup unit (gu_poly_*).
geom gu (
    .clk                ( clk ),
    .reset              ( cpu_reset ),
    .wr                 ( gu_wr ),
    .wr_addr            ( d_addr[6:2] ),
    .wr_data            ( d_data ),
    .busy               ( gu_busy ),
    .full               ( gu_full ),
    .faces              ( gu_faces ),
    .busy_clocks        ( gu_busy_clocks ),
    .mem_valid          ( gu_mem_valid ),
    .mem_ready          ( gu_mem_ready ),
    .mem_addr           ( gu_mem_addr ),
    .mem_rsp_valid      ( gu_mem_rsp_valid ),
    .mem_rsp_data       ( rs_mem_rsp_data ),
    .poly_valid         ( gu_poly_valid ),
    .poly_ready         ( gu_poly_ready ),
    .poly_n             ( gu_poly_n ),
    .poly_mode          ( gu_poly_mode ),
    .poly_tex           ( gu_poly_tex ),
    .poly_size          ( gu_poly_size ),
    .poly_c             ( gu_poly_c )
);

// The setup unit (setup.v): registers at 0xF000_3080. It takes polygons from the geometry
// unit or from the CPU's corner writes and hands triangle set-ups (su_*) to the rasterizer.
setup su (
    .clk                ( clk ),
    .reset              ( cpu_reset ),
    .wr                 ( su_wr ),
    .wr_addr            ( d_addr[5:2] ),
    .wr_data            ( d_data ),
    .ready              ( su_ready ),
    .busy               ( su_busy ),
    .busy_clocks        ( su_busy_clocks ),
    .go                 ( su_go ),
    .rs_accept          ( rs_su_accept ),
    .t_v0               ( su_v0 ),
    .t_v1               ( su_v1 ),
    .t_v2               ( su_v2 ),
    .t_ref              ( su_ref ),
    .t_tex              ( su_tex ),
    .t_size             ( su_size ),
    .t_draw             ( su_draw ),
    .t_a0               ( su_a0 ),
    .t_a1               ( su_a1 ),
    .t_a2               ( su_a2 ),
    .t_dx0              ( su_dx0 ),
    .t_dx1              ( su_dx1 ),
    .t_dx2              ( su_dx2 ),
    .t_dy0              ( su_dy0 ),
    .t_dy1              ( su_dy1 ),
    .t_dy2              ( su_dy2 ),
    .gu_valid           ( gu_poly_valid ),
    .gu_ready           ( gu_poly_ready ),
    .gu_n               ( gu_poly_n ),
    .gu_mode            ( gu_poly_mode ),
    .gu_tex             ( gu_poly_tex ),
    .gu_size            ( gu_poly_size ),
    .gu_c               ( gu_poly_c )
);

// The rasterizer (raster.v): registers at 0xF000_3000. It draws the set-ups of the setup
// unit (or triangles from the CPU's registers) into the framebuffer.
raster rs (
    .clk                ( clk ),
    .reset              ( cpu_reset ),
    .wr                 ( rs_wr ),
    .wr_addr            ( d_addr[6:2] ),
    .wr_data            ( d_data ),
    .busy               ( rs_busy ),
    .pixels             ( rs_pixels ),
    .misses             ( rs_misses ),
    .busy_clocks        ( rs_busy_clocks ),
    .miss_clocks        ( rs_miss_clocks ),
    .edge_clocks        ( rs_edge_clocks ),
    .row_clocks         ( rs_row_clocks ),
    .drain_clocks       ( rs_drain_clocks ),
    .fb_wr              ( rs_fb_wr ),
    .fb_buf             ( rs_fb_buf ),
    .fb_addr            ( rs_fb_addr ),
    .fb_data            ( rs_fb_data ),
    .fb_be              ( rs_fb_be ),
    .fb_q               ( fb_rd_data ),             // blended pixels read the framebuffer
    .mem_valid          ( rs_mem_valid ),
    .mem_ready          ( rs_mem_ready ),
    .mem_addr           ( rs_mem_addr ),
    .mem_rsp_valid      ( rs_mem_rsp_valid ),
    .mem_rsp_data       ( rs_mem_rsp_data ),
    .su_go              ( su_go ),
    .su_accept          ( rs_su_accept ),
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

    // A command is accepted only when no read is in progress, sdram_port can take a command
    // (whatever its target) and no unit makes the command wait.
    assign dbus_cmd_ready = d_state == D_IDLE && !cpu_reset && sdp_db_cmd_ready && !rs_waits;
    wire        d_fire = dbus_cmd_valid && dbus_cmd_ready;
    assign sdp_db_cmd_valid = d_fire && cmd_region == R_SDRAM;

    // One source answers at a time.
    assign dbus_rsp_valid = d_rsp_valid || sdp_db_rsp_valid;
    assign dbus_rsp_data = sdp_db_rsp_valid ? sdp_db_rsp_data : d_rsp_data;
    assign dbus_rsp_last = sdp_db_rsp_valid ? sdp_db_rsp_last : d_rsp_last;
    assign dbus_rsp_error = sdp_db_rsp_valid ? sdp_db_rsp_error : d_rsp_error;

    // The framebuffer port belongs to the rasterizer while it draws, then to the blitter
    // while it draws. Otherwise it follows the command being worked on, like the RAM ports
    // below: a posted write goes out in the clock after its command, and so does a read's
    // address. The framebuffer words hold the leftmost pixel in bits 31:24, so the data is
    // byte swapped and the byte mask reversed.
    assign fb_wr_en = rs_busy ? rs_fb_wr : bl_busy ? bl_fb_wr : d_write && d_region == R_FB;
    assign fb_buf = rs_busy ? rs_fb_buf : bl_busy ? bl_fb_buf : d_addr[16];
    assign fb_addr = rs_busy ? rs_fb_addr : bl_busy ? bl_fb_addr : d_addr[15:2];
    assign fb_wr_data = rs_busy ? rs_fb_data : bl_busy ? bl_fb_data : swap32(d_data);
    assign fb_wr_be = rs_busy ? rs_fb_be : bl_busy ? bl_fb_be : {d_mask[0], d_mask[1], d_mask[2], d_mask[3]};

    // Port B of the boot RAM and port A of the debug log, driven by the command being worked
    // on. The boot RAM's port B belongs to the bridge while the CPU is in reset, else to the
    // dBus.
always @(*) begin
    boot_we_b = 4'b0000;
    boot_wdata_b = d_data;
    boot_addr_b = d_state == D_LINE ? {d_addr[14:5], d_cnt} : d_addr[14:2];
    if(cpu_reset) begin
        boot_addr_b = bridge_addr[14:2];
        boot_wdata_b = swap32(bridge_wr_data);
        if(bridge_wr && bridge_addr[31:15] == 0) boot_we_b = 4'b1111;
    end else if(d_write && d_region == R_BOOT) begin
        boot_we_b = d_mask;
    end
    log_addr_a = d_addr[11:2];
    log_wdata_a = d_data;
    log_we_a = d_write && d_region == R_LOG ? d_mask : 4'b0000;
end

    // register state
    reg  [1:0]  ack_s = 0;                      // swap acknowledge, synchronised from clk_vid
    reg  [2:0]  frame_s = 0;                    // frame tick, synchronised from clk_vid
    reg  [15:0] frame_count = 0;                // video frames (VID_STATUS bits 31:16)
    reg  [31:0] key_r, joy_r;                   // controller inputs, registered
    reg  [15:0] trig_r;
    wire        swap_pending = swap_req_toggle != ack_s[1];
    reg         ds_started = 0;                 // the data slot read's command has begun
    reg  [2:0]  ds_result = 0;                  // the last one's result, 0 = ok
    // state of the data slot table's look-ups (see the last always block)
    reg  [2:0]  dt_state = 0;                   // 0 idle, 1-3 an id word, 4-5 a size word
    reg  [4:0]  dt_slot;                        // the slot being looked at
    reg  [1:0]  dt_wait;                        // clocks left until the read data is there
    reg         dt_log;                         // looking for the debug log's slot, else ds_id
    reg         dt_lookup = 0;                  // DS_ID written: a look-up for ds_id waits
    reg         dt_put = 0;                     // DS_SIZE written: the look-up writes dt_value
    reg  [31:0] dt_value;                       // the size that DS_SIZE was written with
    reg  [31:0] ds_size = 0;                    // DS_SIZE: the size found for ds_id
    wire        ds_id_wr = d_write && d_region == R_REG && d_addr[15:0] == 16'h0020;
    wire        ds_size_wr = d_write && d_region == R_REG && d_addr[15:0] == 16'h0034;
    wire        ds_sizing = dt_lookup || dt_state != 3'd0;
    reg  [31:0] reg_q;                          // register read data, valid one clock after d_addr
    wire        writes_idle = sdram_cpu_writes_idle;

    // Synchronisers and input registers, and the register read multiplexer (docs/soc.md,
    // Registers). reg_q follows d_addr one clock later, which is the data phase of a single
    // read.
always @(posedge clk) begin
    ack_s <= {ack_s[0], swap_ack_toggle};
    frame_s <= {frame_s[1:0], frame_toggle};
    if(frame_s[2] != frame_s[1]) frame_count <= frame_count + 1'b1;
    key_r <= cont1_key;
    joy_r <= cont1_joy;
    trig_r <= cont1_trig;

    case(d_addr[15:0])
        16'h0000: reg_q <= SYS_ID;
        16'h0008: reg_q <= {31'd0, sim};
        16'h000C: reg_q <= {27'd0, writes_idle, br_dropped, br_overflow, sdram_ready, sdram_locked};
        16'h0010: reg_q <= {8'd0, br_words};
        16'h0014: reg_q <= sdram_refreshes;
        16'h0040: reg_q <= opt_music_r;
        16'h0018: reg_q <= {phase_count, 14'd0, sdram_ready, phase_busy || phase_step};
        16'h0030: reg_q <= {25'd0, ds_result, 2'd0, ds_sizing, ds_read};
        16'h0034: reg_q <= ds_size;
        16'h1000: reg_q <= {frame_count, 14'd0, swap_pending, ack_s[1]};
        16'h2000: reg_q <= key_r;
        16'h2004: reg_q <= joy_r;
        16'h2008: reg_q <= {16'd0, trig_r};
        16'h3014: reg_q <= {31'd0, rs_busy || su_busy || gu_busy || bl_busy};
        16'h3018: reg_q <= rs_pixels;
        16'h3050: reg_q <= rs_misses;
        16'h3054: reg_q <= rs_busy_clocks;
        16'h3058: reg_q <= rs_miss_clocks;
        16'h305C: reg_q <= rs_edge_clocks;
        16'h3060: reg_q <= rs_row_clocks;
        16'h3064: reg_q <= rs_drain_clocks;
        16'h3068: reg_q <= rs_starved;
        16'h30AC: reg_q <= su_busy_clocks;
        16'h3150: reg_q <= gu_faces;
        16'h3154: reg_q <= gu_busy_clocks;
        16'h3184: reg_q <= bl_busy_clocks;
        16'h4400: reg_q <= au_playing;
        16'h4408: reg_q <= {31'd0, au_late};
        16'h4414: reg_q <= {15'd0, au_music};
        16'h5000: reg_q <= {31'd0, fm_run};
        16'h5008: reg_q <= {14'd0, fm_busy, fm_ready, 7'd0, fm_fill};
        16'h500C: reg_q <= {16'd0, fm_now};
        default:  reg_q <= 32'd0;
    endcase
end

    // The dBus state machine: accepts a command (d_fire), carries out posted writes and
    // register side effects, and answers single and line reads. The one-clock strobes are
    // cleared first and set again below where needed. Also runs the data slot read handshake.
always @(posedge clk) begin
    pal_wr <= 0;
    d_rsp_valid <= 0;
    d_write <= 0;
    d_issued <= 0;
    phase_step <= 0;
    sdram_reinit <= 0;

    // A data slot read is done when core_bridge_cmd's done has gone low (the command
    // started) and is high again.
    if(ds_read) begin
        if(!ds_done) ds_started <= 1;
        else if(ds_started) begin
            ds_read <= 0;
            ds_result <= ds_err;
        end
    end

    if(cpu_reset) begin
        d_state <= D_IDLE;
        swap_req_toggle <= 0;               // video_out's acknowledge restarts at 0 too
        sim_status <= 0;
        ds_read <= 0;
    end else begin
        // A command: remember it. A write happens in the next clock. A read starts a state
        // that blocks further commands until its last word is out.
        if(d_fire) begin
            d_addr <= dbus_cmd_addr;
            d_data <= dbus_cmd_data;
            d_mask <= dbus_cmd_mask;
            d_wr <= dbus_cmd_wr;
            d_region <= cmd_region;
            if(dbus_cmd_wr) d_write <= 1;
            else if(cmd_region == R_SDRAM) d_state <= D_EXT;
            else if(dbus_cmd_size == 3'd5) begin d_state <= D_LINE; d_cnt <= 0; end
            else begin d_state <= D_SINGLE; d_phase <= 0; end
        end

        // The posted write. Writes to the boot RAM, the log and the framebuffer go out through
        // the RAM ports above, and writes to the 3D units, the blitter, the mixer and the FM
        // chip through the strobes decoded above. Here are the system and video registers.
        if(d_write) begin
            case(d_region)
            R_REG: begin
                if(d_addr[15:0] == 16'h0004) sim_status <= d_data;           // SYS_SIM
                if(d_addr[15:0] == 16'h0018) begin                           // MEM_PHASE
                    phase_step <= d_data[0] && !phase_busy;
                    phase_later <= d_data[1];
                    sdram_reinit <= d_data[8];
                end
                if(d_addr[15:0] == 16'h0020) ds_id <= d_data[15:0];          // DS_ID
                if(d_addr[15:0] == 16'h0024) ds_offset <= d_data;            // DS_OFFSET
                if(d_addr[15:0] == 16'h0028) ds_addr <= d_data;              // DS_ADDR
                if(d_addr[15:0] == 16'h002C) ds_length <= d_data;            // DS_LENGTH
                if(d_addr[15:0] == 16'h0030 && d_data[0] && !ds_read) begin  // DS_CTRL: start
                    ds_read <= 1;
                    ds_started <= 0;
                end
                // VID_SWAP: a request toggles the line, unless one is pending
                if(d_addr[15:0] == 16'h1004 && !swap_pending) swap_req_toggle <= ~swap_req_toggle;
                if(d_addr[15:10] == 6'b000101) begin        // VID_PAL, 0x1400-0x17FC
                    pal_wr <= 1;
                    pal_buf <= ~swap_req_toggle;            // the back buffer
                    pal_addr <= d_addr[9:2];
                    pal_data <= d_data[23:0];
                end
            end
            default: ;
            endcase
        end

        // Single read (D_SINGLE): the address goes out in phase 0, the data comes back in
        // phase 1. The answer has the error flag when the address is in no region.
        if(d_state == D_SINGLE) begin
            d_phase <= 1;
            if(d_phase) begin
                d_rsp_valid <= 1;
                d_rsp_last <= 1;
                d_rsp_error <= d_region == R_NONE;
                case(d_region)
                    R_BOOT:  d_rsp_data <= boot_q_b;
                    R_LOG:   d_rsp_data <= log_q_a;
                    R_FB:    d_rsp_data <= swap32(fb_rd_data);
                    default: d_rsp_data <= reg_q;
                endcase
                d_state <= D_IDLE;
            end
        end

        // Line read (D_LINE): 8 words from the boot RAM, one a clock; any other region
        // answers with error words. A word is addressed in one clock (d_issued) and
        // answered in the next.
        if(d_state == D_LINE) begin
            d_issued <= 1;
            d_cnt <= d_cnt + 1'b1;
            if(d_cnt == 3'd7) d_state <= D_IDLE;
        end
        if(d_issued) begin
            d_rsp_valid <= 1;
            d_rsp_last <= d_cnt == 3'd0;            // the eighth word (d_cnt wrapped)
            d_rsp_error <= d_region != R_BOOT;
            d_rsp_data <= boot_q_b;
        end

        // Reads from the SDRAM (D_EXT): the words come straight from sdram_port. The state
        // ends with the last word.
        if(d_state == D_EXT && sdp_db_rsp_valid && sdp_db_rsp_last) d_state <= D_IDLE;
    end
end

//
// The APF's data slot table (core_bridge_cmd). For each slot it holds the slot's id, then
// its file's size. The APF fills it in when it loads the core, deferload slots too.
//
// This state machine does three look-ups, each a scan over the 32 table entries:
//   - After reset: give the debug log's slot its full size (LOG_SIZE), so the APF saves all
//     of it on exit. The APF sets a size only when it loaded an existing file.
//   - After each write of DS_ID: find that slot and put its size into DS_SIZE. DS_SIZE is
//     0 if the table has no such slot. DS_CTRL bit 1 shows that the look-up is running.
//   - After a write of DS_SIZE: store that value as the size of slot DS_ID. This is for the
//     saved games in the SDRAM, which the APF writes back on exit only if the slot has a size.
//
// States: 0 idle; 1 address the id word of dt_slot; 2 wait for it; 3 compare it, then
// either handle the size word or go to the next slot; 4 wait for the size word; 5 take it
// into ds_size.
//
    // table look-ups
always @(posedge clk) begin
    dt_wren <= 0;
    if(cpu_reset) begin
        dt_state <= 1;
        dt_slot <= 0;
        dt_log <= 1;
        dt_lookup <= 0;
        dt_put <= 0;
    end else begin
        case(dt_state)
        0: if(dt_lookup) begin                  // a look-up for ds_id
            dt_lookup <= 0;
            dt_log <= 0;
            dt_slot <= 0;
            dt_state <= 1;
        end
        1: begin                                // address the slot's id word
            dt_addr <= {4'd0, dt_slot, 1'b0};
            dt_wait <= 2;
            dt_state <= 2;
        end
        2: begin                                // wait for the read data
            dt_wait <= dt_wait - 1'b1;
            if(dt_wait == 0) dt_state <= 3;
        end
        3: begin                                // compare the id with the wanted slot
            if(dt_q[15:0] == (dt_log ? LOG_SLOT_ID : ds_id)) begin
                dt_addr <= {4'd0, dt_slot, 1'b1};   // its size word
                dt_data <= dt_log ? LOG_SIZE : dt_value;
                dt_wren <= dt_log || dt_put;
                dt_wait <= 2;
                dt_state <= dt_log || dt_put ? 3'd0 : 3'd4;
                if(dt_put) ds_size <= dt_value;
                dt_put <= 0;
            end else begin
                dt_slot <= dt_slot + 1'b1;
                if(dt_slot == 5'd31 && !dt_log) ds_size <= 0;
                if(dt_slot == 5'd31) dt_put <= 0;
                dt_state <= dt_slot == 5'd31 ? 3'd0 : 3'd1;
            end
        end
        4: begin                                // wait for the size
            dt_wait <= dt_wait - 1'b1;
            if(dt_wait == 0) dt_state <= 5;
        end
        5: begin                                // the size is there
            ds_size <= dt_q;
            dt_state <= 0;
        end
        default: dt_state <= 0;
        endcase
        if(ds_id_wr || ds_size_wr) dt_lookup <= 1;  // after the case: a write wins over the start
        if(ds_id_wr) dt_put <= 0;
        if(ds_size_wr) begin
            dt_put <= 1;
            dt_value <= d_data;
        end
    end
end

endmodule
