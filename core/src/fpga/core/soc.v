//
// The CPU system of step 1 (docs/soc.md): VexRiscv, 16 KB boot RAM, registers, the
// framebuffer drawing port, the 4 KB debug log and a CFU
//
// One clock for everything (clk_74a in step 1). The CPU is held in reset while reset_n
// is low; the APF loads the firmware data slot into the boot RAM before it releases
// reset. The bridge delivers big-endian words, the CPU is little-endian: bridge data
// into and out of the CPU's memories is byte-swapped, and so is the framebuffer, whose
// words keep the leftmost pixel in [31:24].
//
// Bridge reads are answered one read later, as the APF expects (io_bridge_peripheral.v:
// "reads are buffered by 1 word"): the data for the address of a read is latched at its
// bridge_rd pulse and sent in the next read transaction. The APF reads one word more
// than it needs and drops the first answer. core_bridge_cmd answers the same way.
//
// Buses: VexRiscv's native cache buses.
//   iBus  a command per 32-byte line, answered with 8 words in order (boot RAM only)
//   dBus  line reads (size 5, 8 words), single reads (1 word, 2 clocks later) and
//         single writes (byte mask, no answer). A read blocks the next command until
//         its last word is out; writes are posted and take one command a clock.
//

module soc (
    input   wire            clk,
    input   wire            reset_n,

    // APF bridge (firmware into the boot RAM, debug log in and out), synchronous to clk
    input   wire            bridge_wr,
    input   wire            bridge_rd,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,
    output  reg     [31:0]  bridge_log_rd_data,     // read data for bridge addresses 0x0010_xxxx

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

    // video_out: palette of the back buffer, swap handshake, frame tick
    output  reg             pal_wr = 0,
    output  reg             pal_buf,
    output  reg     [7:0]   pal_addr,
    output  reg     [23:0]  pal_data,
    output  reg             swap_req_toggle = 0,
    input   wire            swap_ack_toggle,        // clk_vid domain
    input   wire            frame_toggle,           // clk_vid domain

    // core side of the APF data slot table (core_bridge_cmd), read data 2 clocks later
    output  reg     [9:0]   dt_addr,
    output  reg             dt_wren = 0,
    output  reg     [31:0]  dt_data,
    input   wire    [31:0]  dt_q,

    output  reg     [31:0]  sim_status = 0          // SYS_SIM, for the testbench
);

    localparam  [31:0]  SYS_ID = 32'h53435231;      // "SCR1"
    localparam  [15:0]  LOG_SLOT_ID = 16'd1;        // data slot of debug.txt (core/data.json)
    localparam  [31:0]  LOG_SIZE = 32'd4096;

    function [31:0] swap32(input [31:0] d);
        swap32 = {d[7:0], d[15:8], d[23:16], d[31:24]};
    endfunction

    reg         cpu_reset = 1;
always @(posedge clk) cpu_reset <= ~reset_n;

//
// the CPU
//
    wire        ibus_cmd_valid;
    wire        ibus_cmd_ready;
    wire [31:0] ibus_cmd_addr;
    reg         ibus_rsp_valid = 0;
    reg  [31:0] ibus_rsp_data;
    reg         ibus_rsp_error;

    wire        dbus_cmd_valid;
    wire        dbus_cmd_ready;
    wire        dbus_cmd_wr;
    wire [31:0] dbus_cmd_addr;
    wire [31:0] dbus_cmd_data;
    wire [3:0]  dbus_cmd_mask;
    wire [2:0]  dbus_cmd_size;
    reg         dbus_rsp_valid = 0;
    reg         dbus_rsp_last;
    reg  [31:0] dbus_rsp_data;
    reg         dbus_rsp_error;

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
// boot RAM: 4096 words, port A for the iBus, port B for the dBus (or the bridge while
// the CPU is held in reset)
//
    reg  [11:0] boot_addr_a;
    wire [31:0] boot_q_a, boot_q_b;
    reg  [11:0] boot_addr_b;
    reg  [3:0]  boot_we_b;
    reg  [31:0] boot_wdata_b;

    genvar lane;
    generate
        for(lane = 0; lane < 4; lane = lane + 1) begin : boot_lanes
            ram_lane #(.ADDR_W(12)) ram (
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
// debug log: 1024 words, port A for the dBus, port B for the bridge
//
    reg  [9:0]  log_addr_a;
    reg  [3:0]  log_we_a;
    reg  [31:0] log_wdata_a;
    wire [31:0] log_q_a, log_q_b;
    wire        bridge_log = bridge_addr[31:12] == 20'h00100;
    wire [31:0] bridge_wdata_le = swap32(bridge_wr_data);

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
                .we_b       ( bridge_wr & bridge_log ),
                .wdata_b    ( bridge_wdata_le[lane*8 +: 8] ),
                .q_b        ( log_q_b[lane*8 +: 8] )
            );
        end
    endgenerate

    // the answer for the next read transaction (see the header)
always @(posedge clk) begin
    if(bridge_rd && bridge_log) bridge_log_rd_data <= swap32(log_q_b);
end

//
// iBus: line reads from the boot RAM, one word a clock
//
    reg         ib_busy = 0;
    reg         ib_ok;                      // the line is inside the boot RAM
    reg  [8:0]  ibus_line;
    reg  [2:0]  ib_cnt;
    reg         ib_issued = 0;              // a word was addressed last clock
    reg         ib_issued_ok;

    assign ibus_cmd_ready = !ib_busy && !cpu_reset;

always @(*) begin
    boot_addr_a = ib_busy ? {ibus_line, ib_cnt} : ibus_cmd_addr[13:2];
end

always @(posedge clk) begin
    ibus_rsp_valid <= ib_issued;
    ibus_rsp_data <= boot_q_a;
    ibus_rsp_error <= !ib_issued_ok;
    ib_issued <= 0;
    if(cpu_reset) begin
        ib_busy <= 0;
    end else if(!ib_busy) begin
        if(ibus_cmd_valid) begin
            ib_busy <= 1;
            ib_ok <= ibus_cmd_addr[31:14] == 0;
            ibus_line <= ibus_cmd_addr[13:5];
            ib_cnt <= 0;
        end
    end else begin
        ib_issued <= 1;
        ib_issued_ok <= ib_ok;
        ib_cnt <= ib_cnt + 1'b1;
        if(ib_cnt == 3'd7) ib_busy <= 0;
    end
end

//
// dBus
//
    localparam  R_NONE  = 3'd0;
    localparam  R_BOOT  = 3'd1;
    localparam  R_LOG   = 3'd2;
    localparam  R_FB    = 3'd3;
    localparam  R_REG   = 3'd4;

    function [2:0] region(input [31:0] a);
        if(a[31:14] == 0)                                   region = R_BOOT;
        else if(a[31:12] == 20'hF0008)                      region = R_LOG;
        else if(a[31:17] == 15'h7880 && a[15:2] < 14'd16000) region = R_FB;
        else if(a[31:12] == 20'hF0000 || a[31:12] == 20'hF0001 || a[31:12] == 20'hF0002)
                                                            region = R_REG;
        else                                                region = R_NONE;
    endfunction

    localparam  D_IDLE   = 2'd0;
    localparam  D_SINGLE = 2'd1;
    localparam  D_LINE   = 2'd2;

    reg  [1:0]  d_state = D_IDLE;
    reg  [31:0] d_addr;                     // the command being worked on
    reg  [31:0] d_data;
    reg  [3:0]  d_mask;
    reg  [2:0]  d_region;
    reg         d_write = 0;                // a posted write to do this clock
    reg  [2:0]  d_cnt;
    reg         d_phase;                    // single read: 0 = address out, 1 = data in
    reg         d_issued = 0;               // line read: a word was addressed last clock

    assign dbus_cmd_ready = d_state == D_IDLE && !cpu_reset;
    wire        d_fire = dbus_cmd_valid && dbus_cmd_ready;

    // the framebuffer port follows the command being worked on, like the RAM ports below:
    // a posted write goes out in the clock after its command, a read's address too
    assign fb_wr_en = d_write && d_region == R_FB;
    assign fb_buf = d_addr[16];
    assign fb_addr = d_addr[15:2];
    assign fb_wr_data = swap32(d_data);
    assign fb_wr_be = {d_mask[0], d_mask[1], d_mask[2], d_mask[3]};

    // the boot RAM's port B: bridge while the CPU is in reset, else the dBus
always @(*) begin
    boot_we_b = 4'b0000;
    boot_wdata_b = d_data;
    boot_addr_b = d_state == D_LINE ? {d_addr[13:5], d_cnt} : d_addr[13:2];
    if(cpu_reset) begin
        boot_addr_b = bridge_addr[13:2];
        boot_wdata_b = swap32(bridge_wr_data);
        if(bridge_wr && bridge_addr[31:14] == 0) boot_we_b = 4'b1111;
    end else if(d_write && d_region == R_BOOT) begin
        boot_we_b = d_mask;
    end
    log_addr_a = d_addr[11:2];
    log_wdata_a = d_data;
    log_we_a = d_write && d_region == R_LOG ? d_mask : 4'b0000;
end

    // registers
    reg  [1:0]  ack_s = 0;
    reg  [2:0]  frame_s = 0;
    reg  [15:0] frame_count = 0;
    reg  [31:0] key_r, joy_r;
    reg  [15:0] trig_r;
    wire        swap_pending = swap_req_toggle != ack_s[1];
    reg  [31:0] reg_q;

always @(posedge clk) begin
    ack_s <= {ack_s[0], swap_ack_toggle};
    frame_s <= {frame_s[1:0], frame_toggle};
    if(frame_s[2] != frame_s[1]) frame_count <= frame_count + 1'b1;
    key_r <= cont1_key;
    joy_r <= cont1_joy;
    trig_r <= cont1_trig;

    case(d_addr[15:0])
        16'h0000: reg_q <= SYS_ID;
        16'h1000: reg_q <= {frame_count, 14'd0, swap_pending, ack_s[1]};
        16'h2000: reg_q <= key_r;
        16'h2004: reg_q <= joy_r;
        16'h2008: reg_q <= {16'd0, trig_r};
        default:  reg_q <= 32'd0;
    endcase
end

always @(posedge clk) begin
    pal_wr <= 0;
    dbus_rsp_valid <= 0;
    d_write <= 0;
    d_issued <= 0;

    if(cpu_reset) begin
        d_state <= D_IDLE;
    end else begin
        // a command: remember it; a write happens next clock, a read blocks
        if(d_fire) begin
            d_addr <= dbus_cmd_addr;
            d_data <= dbus_cmd_data;
            d_mask <= dbus_cmd_mask;
            d_region <= region(dbus_cmd_addr);
            if(dbus_cmd_wr) d_write <= 1;
            else if(dbus_cmd_size == 3'd5) begin d_state <= D_LINE; d_cnt <= 0; end
            else begin d_state <= D_SINGLE; d_phase <= 0; end
        end

        // the posted write
        if(d_write) begin
            case(d_region)
            R_REG: begin
                if(d_addr[15:0] == 16'h0004) sim_status <= d_data;
                if(d_addr[15:0] == 16'h1004 && !swap_pending) swap_req_toggle <= ~swap_req_toggle;
                if(d_addr[15:10] == 6'b000101) begin        // 0x1400-0x17FC
                    pal_wr <= 1;
                    pal_buf <= ~swap_req_toggle;            // the back buffer
                    pal_addr <= d_addr[9:2];
                    pal_data <= d_data[23:0];
                end
            end
            default: ;
            endcase
        end

        // single read: the address goes out in phase 0, the data comes back in phase 1
        if(d_state == D_SINGLE) begin
            d_phase <= 1;
            if(d_phase) begin
                dbus_rsp_valid <= 1;
                dbus_rsp_last <= 1;
                dbus_rsp_error <= d_region == R_NONE;
                case(d_region)
                    R_BOOT:  dbus_rsp_data <= boot_q_b;
                    R_LOG:   dbus_rsp_data <= log_q_a;
                    R_FB:    dbus_rsp_data <= swap32(fb_rd_data);
                    default: dbus_rsp_data <= reg_q;
                endcase
                d_state <= D_IDLE;
            end
        end

        // line read: 8 words from the boot RAM, one a clock (error words elsewhere)
        if(d_state == D_LINE) begin
            d_issued <= 1;
            d_cnt <= d_cnt + 1'b1;
            if(d_cnt == 3'd7) d_state <= D_IDLE;
        end
        if(d_issued) begin
            dbus_rsp_valid <= 1;
            dbus_rsp_last <= d_cnt == 3'd0;             // the eighth word (d_cnt wrapped)
            dbus_rsp_error <= d_region != R_BOOT;
            dbus_rsp_data <= boot_q_b;
        end
    end
end

//
// after reset: give the debug log's data slot its full size in the slot table, so the
// APF saves all of it on exit (it only sets a size when it loaded an existing file)
//
    reg  [2:0]  dt_state = 0;
    reg  [4:0]  dt_slot;
    reg  [1:0]  dt_wait;

always @(posedge clk) begin
    dt_wren <= 0;
    if(cpu_reset) begin
        dt_state <= 1;
        dt_slot <= 0;
    end else case(dt_state)
    1: begin                                    // address the slot's id word
        dt_addr <= {4'd0, dt_slot, 1'b0};
        dt_wait <= 2;
        dt_state <= 2;
    end
    2: begin                                    // wait for the read data
        dt_wait <= dt_wait - 1'b1;
        if(dt_wait == 0) dt_state <= 3;
    end
    3: begin
        if(dt_q[15:0] == LOG_SLOT_ID) begin
            dt_addr <= {4'd0, dt_slot, 1'b1};   // its size word
            dt_data <= LOG_SIZE;
            dt_wren <= 1;
            dt_state <= 0;
        end else begin
            dt_slot <= dt_slot + 1'b1;
            dt_state <= dt_slot == 5'd31 ? 3'd0 : 3'd1;
        end
    end
    default: ;
    endcase
end

endmodule
