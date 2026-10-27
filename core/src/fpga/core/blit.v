//
// Blitter: the race screen's pictures, characters and needles into the framebuffer, bit for
// bit the C model firmware/blit.c. The CPU writes commands into a buffer of 512 words (128
// commands) and goes on; the blitter takes one once the 3D units are done (go), so the race
// screen lies over the scene. One pixel a clock: a picture's or a character's bytes come from
// the SDRAM a line of 32 bytes at a time (the shared line port), the pixel goes into the
// framebuffer with its byte enable.
//
// Registers (soc.v maps them at 0xF000_3180 + 4 * wr_addr):
//   0       write: the next word of a command (soc.v holds the write back while the buffer is
//           full). A command is 4 words:
//           0  copy, font: SDRAM byte address of the first pixel; line: [9:0] x, [25:16] y of
//              its end (signed)
//           1  [9:0] x, [25:16] y (signed): the first pixel's place on the screen, or the
//              line's start; [29:28] kind (0 copy, 1 font, 2 line); [30] the buffer drawn into
//           2  copy, font: [8:0] width, [24:16] height (1..511); line: the clip's [8:0] left,
//              [24:16] top
//           3  copy, font: [8:0] bytes from one row to the next; line: the clip's [8:0] right,
//              [24:16] bottom (both outside it); [31:24] colour
//           copy: the byte as it is, 0 see-through; font: a shade 1..15 (0 see-through) drawn as
//           colour * 16 + shade; line: Bresenham from start to end, the pixels in the clip in
//           the colour. Copies and characters are cut to the screen (320 x 200).
//   1       clocks busy since the last clear (write: clear)
//

module blit (
    input   wire            clk,
    input   wire            reset,

    input   wire            wr,
    input   wire    [4:0]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            busy,
    output  wire            full,               // the command buffer takes no word now
    output  reg     [31:0]  busy_clocks = 0,
    input   wire            go,                 // the 3D units are done: a command may start

    // framebuffer writes, one pixel each (be[3] is the leftmost pixel of the word)
    output  reg             fb_wr = 0,
    output  reg             fb_buf,
    output  reg     [13:0]  fb_addr,
    output  reg     [31:0]  fb_data,
    output  reg     [3:0]   fb_be,

    // lines from the SDRAM (shared with the rasterizer's port, soc.v)
    output  reg             mem_valid = 0,
    input   wire            mem_ready,
    output  reg     [25:5]  mem_addr,
    input   wire            mem_rsp_valid,
    input   wire    [31:0]  mem_rsp_data
);

//
// the command buffer: 512 words, block RAM
//
    (* ramstyle = "no_rw_check, M10K" *) reg [31:0] fifo [0:511];
    reg  [9:0]  f_wp = 0, f_rp = 0;
    wire [9:0]  f_count = f_wp - f_rp;
    reg  [8:0]  f_ra;
    reg  [31:0] f_q;
    assign full = f_count >= 10'd508;
    wire        push = wr && wr_addr == 5'd0 && !full;
always @(posedge clk) begin
    if(push) fifo[f_wp[8:0]] <= wr_data;
    f_q <= fifo[f_ra];
end

    localparam  [2:0]   B_IDLE = 3'd0, B_FETCH = 3'd1, B_SETUP = 3'd2, B_PIX = 3'd3, B_MEM = 3'd4,
                        B_LINE = 3'd5;
    reg  [2:0]  state = B_IDLE;
    reg  [2:0]  fk;                             // words of the command taken
    reg  [31:0] c0, c1, c2;
    reg  [1:0]  kind;
    reg  [7:0]  colour;

    // copy, font
    reg  [25:0] row_addr;                       // the row's first byte
    reg  [8:0]  w, h, stride, c, r;
    reg  signed [9:0] x0, y0;
    wire signed [10:0] px = $signed({x0[9], x0}) + $signed({2'd0, c});
    wire signed [10:0] py = $signed({y0[9], y0}) + $signed({2'd0, r});
    wire        on_screen = px >= 11'sd0 && px < 11'sd320 && py >= 11'sd0 && py < 11'sd200;
    wire [25:0] a = row_addr + {17'd0, c};
    reg  [255:0] lb;                            // the line in the buffer
    reg  [20:0] lb_tag;
    reg         lb_ok = 0;
    reg  [2:0]  fill;
    wire [7:0]  byte_at = lb[{a[4:0], 3'd0} +: 8];
    wire        last_c = c == w - 9'd1, last_r = r == h - 9'd1;

    // line
    reg  signed [10:0] lx, ly, ex, ey, ldx, ldy;
    reg  signed [12:0] err;
    reg         sxp, syp;                       // the line goes right, down
    reg  [8:0]  cx0, cy0, cx1, cy1;
    wire signed [12:0] e2 = err <<< 1;
    // sign-extended into signed wires: compared as a concatenation they would be unsigned
    wire signed [12:0] sdx = {{2{ldx[10]}}, ldx}, sdy = {{2{ldy[10]}}, ldy};
    wire        in_clip = lx >= $signed({2'd0, cx0}) && lx < $signed({2'd0, cx1}) &&
                          ly >= $signed({2'd0, cy0}) && ly < $signed({2'd0, cy1});

    assign busy = state != B_IDLE || f_count != 10'd0 || fb_wr || mem_valid;
`ifdef BLIT_TRACE
    reg  [31:0] trace_t = 0;
always @(posedge clk) trace_t <= busy ? trace_t + 1'b1 : 32'd0;
`endif

    // a pixel into the framebuffer: word y * 80 + x / 4
    task plot(input signed [10:0] x, input signed [10:0] y, input [7:0] v);
        begin
            fb_wr <= 1;
            fb_addr <= {y[7:0], 6'd0} + {2'd0, y[7:0], 4'd0} + {6'd0, x[9:2]};
            fb_be <= 4'b1000 >> x[1:0];
            fb_data <= {4{v}};
        end
    endtask

always @(posedge clk) begin
    fb_wr <= 0;
    if(push) f_wp <= f_wp + 1'b1;
    if(state != B_IDLE) busy_clocks <= busy_clocks + 1'b1;
    if(wr && wr_addr == 5'd1) busy_clocks <= 0;
    if(mem_valid && mem_ready) mem_valid <= 0;

    case(state)
    B_IDLE: if(go && f_count >= 10'd4) begin
        f_ra <= f_rp[8:0];
        f_rp <= f_rp + 1'b1;
        fk <= 0;
        state <= B_FETCH;
    end

    B_FETCH: begin                              // word fk comes out a clock after it is asked for
        fk <= fk + 1'b1;
        if(fk < 3'd3) begin
            f_ra <= f_rp[8:0];
            f_rp <= f_rp + 1'b1;
        end
        if(fk == 3'd1) c0 <= f_q;
        if(fk == 3'd2) c1 <= f_q;
        if(fk == 3'd3) c2 <= f_q;
        if(fk == 3'd4) begin
            colour <= f_q[31:24];
            kind <= c1[29:28];
            fb_buf <= c1[30];
            x0 <= c1[9:0];
            y0 <= c1[25:16];
            if(c1[29:28] == 2'd2) begin
                lx <= {c1[9], c1[9:0]};
                ly <= {c1[25], c1[25:16]};
                ex <= {c0[9], c0[9:0]};
                ey <= {c0[25], c0[25:16]};
                cx0 <= c2[8:0];
                cy0 <= c2[24:16];
                cx1 <= f_q[8:0];
                cy1 <= f_q[24:16];
            end else begin
                row_addr <= c0[25:0];
                w <= c2[8:0];
                h <= c2[24:16];
                stride <= f_q[8:0];
            end
            c <= 0;
            r <= 0;
            state <= B_SETUP;
        end
    end

    B_SETUP: begin
        if(kind == 2'd2) begin
            sxp <= ex >= lx;
            syp <= ey >= ly;
            ldx <= ex >= lx ? ex - lx : lx - ex;                    // |dx|
            ldy <= ey >= ly ? ly - ey : ey - ly;                    // -|dy|
            err <= (ex >= lx ? {{2{ex[10]}}, ex} - {{2{lx[10]}}, lx} : {{2{lx[10]}}, lx} - {{2{ex[10]}}, ex}) +
                   (ey >= ly ? {{2{ly[10]}}, ly} - {{2{ey[10]}}, ey} : {{2{ey[10]}}, ey} - {{2{ly[10]}}, ly});
            state <= B_LINE;
        end else
            state <= w == 9'd0 || h == 9'd0 ? B_IDLE : B_PIX;
    end

    B_PIX: begin
        if(on_screen && !(lb_ok && lb_tag == a[25:5])) begin    // its line first
            mem_valid <= 1;
            mem_addr <= a[25:5];
            fill <= 0;
            lb_ok <= 0;
            state <= B_MEM;
        end else begin
            if(on_screen) begin
                if(kind == 2'd1) begin
                    if(byte_at[3:0] != 4'd0) plot(px, py, {colour[3:0], byte_at[3:0]});
                end else if(byte_at != 8'd0) plot(px, py, byte_at);
            end
            c <= last_c ? 9'd0 : c + 1'b1;
            if(last_c) begin
                r <= r + 1'b1;
                row_addr <= row_addr + {17'd0, stride};
                if(last_r) state <= B_IDLE;
            end
        end
    end

    B_MEM: if(!mem_valid && mem_rsp_valid) begin
        lb[{fill, 5'd0} +: 32] <= mem_rsp_data;
        fill <= fill + 1'b1;
        if(fill == 3'd7) begin
            lb_tag <= mem_addr;
            lb_ok <= 1;
            state <= B_PIX;
        end
    end

    B_LINE: begin                               // a pixel a clock, Bresenham
        if(in_clip) plot(lx, ly, colour);
        if(lx == ex && ly == ey) state <= B_IDLE;
        else begin
            if(e2 >= sdy) lx <= sxp ? lx + 11'sd1 : lx - 11'sd1;
            if(e2 <= sdx) ly <= syp ? ly + 11'sd1 : ly - 11'sd1;
            err <= err + (e2 >= sdy ? sdy : 13'sd0) + (e2 <= sdx ? sdx : 13'sd0);
        end
    end

    default: state <= B_IDLE;
    endcase

`ifdef BLIT_TRACE
    if(state == B_FETCH && fk == 3'd4)
        $display("blit: command %08x %08x %08x %08x", c0, c1, c2, f_q);
    if(busy && trace_t[16:0] == 0)
        $display("blit: state %0d, words %0d, go %0d, mem %0d/%0d", state, f_count, go, mem_valid, mem_ready);
`endif
    if(reset) begin
        state <= B_IDLE;
        f_wp <= 0;
        f_rp <= 0;
        mem_valid <= 0;
        lb_ok <= 0;
        fb_wr <= 0;
    end
end

endmodule
