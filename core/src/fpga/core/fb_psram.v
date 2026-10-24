//
// The 320x200 framebuffer in PSRAM (cram1), fed from the bridge, read line by line
//
// Bridge writes to the data slot window at address 0 (screen.bin, see tools/make_screen.py):
//   0x0000-0x03FF  palette, 256 words of R,G,B,0  -> passed on to video_out
//   0x0400-0xFDFF  pixels, 4 per word, leftmost in [31:24] -> FIFO -> PSRAM
// The bridge cannot be stalled, so its pixel words wait in a FIFO while a line is
// being read.
//
// PSRAM layout: halfword n holds pixels 2n (in [15:8]) and 2n+1, row by row,
// 160 halfwords a line, from halfword 0 of die 0.
//
// When video_out asks for a line (line_req_toggle flips, from the clk_vid domain)
// its 160 halfwords are read into the line buffer. error goes high for good if the
// FIFO ever overflows or a line is not read in time.
//
// Everything here runs on clk_74a and starts from power-up values, not reset_n:
// the bridge loads the data slot while the core is held in reset.
//

module fb_psram #(
    parameter real      CLOCK_MHZ = 74.25,
    parameter integer   T_INIT = 11200          // > 150 us power-up wait of the PSRAM
) (
    input   wire            clk_74a,

    input   wire            bridge_wr,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,

    output  reg             pal_wr = 0,
    output  reg     [7:0]   pal_addr,
    output  reg     [23:0]  pal_data,

    input   wire            line_req_toggle,    // clk_vid domain
    input   wire    [7:0]   line_req_y,         // clk_vid domain, stable while the toggle crosses
    output  reg             lb_wr = 0,
    output  reg     [8:0]   lb_addr,
    output  reg     [15:0]  lb_data,

    output  wire            error,

    output  wire    [21:16] cram_a,
    inout   wire    [15:0]  cram_dq,
    input   wire            cram_wait,
    output  wire            cram_clk,
    output  wire            cram_adv_n,
    output  wire            cram_cre,
    output  wire            cram_ce0_n,
    output  wire            cram_ce1_n,
    output  wire            cram_oe_n,
    output  wire            cram_we_n,
    output  wire            cram_ub_n,
    output  wire            cram_lb_n
);

    localparam  FB_WORDS = 320 * 200 / 4;

//
// bridge: palette straight through, pixel words into the FIFO
//
    reg [45:0]  fifo [0:255];               // {word index, data}
    reg [8:0]   fifo_wp = 0;
    reg [8:0]   fifo_rp = 0;
    wire        fifo_empty = fifo_wp == fifo_rp;
    wire        fifo_full = fifo_wp == {~fifo_rp[8], fifo_rp[7:0]};
    reg         overflow = 0;

    wire        in_window = bridge_addr[31:16] == 16'h0000;
    wire [13:0] fb_word = bridge_addr[15:2] - 14'd256;

always @(posedge clk_74a) begin
    pal_wr <= 0;
    if(bridge_wr && in_window) begin
        if(bridge_addr[15:10] == 6'd0) begin
            pal_wr <= 1;
            pal_addr <= bridge_addr[9:2];
            pal_data <= bridge_wr_data[31:8];
        end else if(fb_word < FB_WORDS) begin
            if(fifo_full) overflow <= 1;
            else begin
                fifo[fifo_wp[7:0]] <= {fb_word, bridge_wr_data};
                fifo_wp <= fifo_wp + 1'b1;
            end
        end
    end
end

//
// line requests from the video clock domain
//
    reg [2:0]   req_s = 0;
    reg         line_pending = 0;
    reg [7:0]   line_y;
    reg         late = 0;

    assign error = overflow | late;

//
// PSRAM sequencer: line reads first, then FIFO words (two halfword writes each)
//
// A request waits in req_* until the controller takes it: it samples read_en /
// write_en in every idle cycle (busy low), so the next request is already there
// in the cycle it becomes free. A halfword access then takes 8 clocks.
//
    localparam  ST_INIT     = 0;
    localparam  ST_IDLE     = 1;
    localparam  ST_FIFO     = 2;
    localparam  ST_WR_HI    = 3;
    localparam  ST_WR_LO    = 4;
    localparam  ST_LINE     = 5;

    reg [2:0]   state = ST_INIT;
    reg [13:0]  init_count = 0;

    reg [45:0]  fifo_q;
    reg [45:0]  cur;                        // the FIFO entry being written
    reg [7:0]   issue_idx;                  // line reads handed to the controller
    reg [7:0]   cap_idx;                    // line reads returned

    reg         req_valid = 0;
    reg         req_write;
    reg [21:0]  req_addr;
    reg [15:0]  req_wdata;
    wire        mem_busy;
    wire        mem_avail;
    wire [15:0] mem_rdata;
    wire        accept = req_valid && !mem_busy;

    wire        reading_line = state == ST_LINE;
    wire        req_arrives = req_s[2] != req_s[1];
    wire        is_late = state != ST_INIT && (line_pending || reading_line);

always @(posedge clk_74a) begin
    lb_wr <= 0;

    req_s <= {req_s[1:0], line_req_toggle};
    if(req_arrives) begin
        if(is_late) late <= 1;
        line_pending <= 1;
        line_y <= line_req_y;
    end

    fifo_q <= fifo[fifo_rp[7:0]];

    // line data back from the controller
    if(reading_line && mem_avail) begin
        lb_wr <= 1;
        lb_addr <= {line_y[0], cap_idx};
        lb_data <= mem_rdata;
        cap_idx <= cap_idx + 1'b1;
    end

    case(state)
    ST_INIT: begin
        init_count <= init_count + 1'b1;
        if(init_count == T_INIT[13:0]) state <= ST_IDLE;
    end
    ST_IDLE: begin
        if(line_pending) begin
            line_pending <= 0;
            req_valid <= 1;
            req_write <= 0;
            req_addr <= {7'd0, line_y, 7'd0} + {9'd0, line_y, 5'd0};     // y * 160
            issue_idx <= 1;
            cap_idx <= 0;
            state <= ST_LINE;
        end else if(!fifo_empty) begin
            state <= ST_FIFO;                                           // fifo_q is valid now
        end
    end
    ST_FIFO: begin
        cur <= fifo_q;
        fifo_rp <= fifo_rp + 1'b1;
        req_valid <= 1;
        req_write <= 1;
        req_addr <= {7'd0, fifo_q[45:32], 1'b0};
        req_wdata <= fifo_q[31:16];
        state <= ST_WR_HI;
    end
    ST_WR_HI: begin
        if(accept) begin
            req_addr <= {7'd0, cur[45:32], 1'b1};
            req_wdata <= cur[15:0];
            state <= ST_WR_LO;
        end
    end
    ST_WR_LO: begin
        if(accept) begin
            req_valid <= 0;
            state <= ST_IDLE;
        end
    end
    ST_LINE: begin
        if(accept) begin
            if(issue_idx == 8'd160) req_valid <= 0;
            else begin
                req_addr <= req_addr + 1'b1;
                issue_idx <= issue_idx + 1'b1;
            end
        end
        if(cap_idx == 8'd160) state <= ST_IDLE;
    end
    endcase
end

// synthesis translate_off
always @(posedge clk_74a) begin
    if(bridge_wr && in_window && bridge_addr[15:10] != 0 && fb_word < FB_WORDS && fifo_full)
        $display("fb_psram: FIFO overflow at %0t", $time);
    if(req_arrives && is_late)
        $display("fb_psram: line %0d requested while line %0d is still being read (%0d of 160 back)",
                 line_req_y, line_y, cap_idx);
end
// synthesis translate_on

psram #(
    .CLOCK_SPEED    ( CLOCK_MHZ )
) cram (
    .clk            ( clk_74a ),
    .bank_sel       ( 1'b0 ),
    .addr           ( req_addr ),
    .write_en       ( accept && req_write ),
    .data_in        ( req_wdata ),
    .write_high_byte( 1'b1 ),
    .write_low_byte ( 1'b1 ),
    .read_en        ( accept && !req_write ),
    .read_avail     ( mem_avail ),
    .data_out       ( mem_rdata ),
    .busy           ( mem_busy ),

    .cram_a         ( cram_a ),
    .cram_dq        ( cram_dq ),
    .cram_wait      ( cram_wait ),
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

endmodule
