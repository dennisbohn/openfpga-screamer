//
// Rasterizer: flat triangles into the framebuffer's drawing port, exactly the pixels of the
// C model firmware/raster.c, with the game's fill rule: pixel (x, r) is drawn when the
// triangle touches the segment from (x, r) to (x, r + 1), its lower end excluded:
//   corners cut to whole pixels after sorting by y (a.y <= b.y <= c.y), rows a.y .. c.y;
//   an edge is evaluated from its upper end, slope = ((x1 - x0) << 16) / (y1 - y0)
//   truncated towards zero, x = (x0 << 12) + slope * (16 r - y0) / 16 in 16.16 pixels at
//   the top of row r, one slope added per row, the corner's x at its lower end; in a row
//   above c.y an edge from xt (top) to xb (bottom) gives [min(ceil xt, floor xb + 1),
//   max(floor xt + 1, ceil xb)), the span from the left edge's start to the right edge's
//   end; row c.y is the corners on it. Limited to the clip rectangle.
// Vertices are signed 16 bit in 1/16 pixel. Setup: one division per edge (16 clocks, two
// quotient bits a clock) and one multiplication, two or three edges a triangle. Then four
// clocks per row and, for one colour, one per framebuffer word: up to four pixels a clock.
// Textured (raster_tex.v): Q, U, V at the span's first pixel from the triangle's reference
// pixel, a = (A << 8) + dx (x0 - xr) + dy (r - yr) (four clocks a row), then one pixel a
// clock into the texture pipeline, a += dx each.
//
// Registers (soc.v maps them at 0xF000_3000 + 4 * wr_addr). Everything but 5, 6 and 19 is
// copied when a triangle starts, so the next one can be set up while one is drawn. A
// triangle from the setup unit (su_*) brings its own values for 0..2, 5 and 7..18.
//   0..2  vertex n: [15:0] x, [31:16] y
//   3     clip: [15:0] x0, [31:16] y0      4  clip: [15:0] x1, [31:16] y1 (exclusive),
//         pixels, limited to the screen
//   5     draw: [7:0] colour, [8] buffer, [9] textured; starts the triangle (soc.v holds
//         this write back while busy)
//   6     clears the pixel counter
//   7     texture: SDRAM byte address of texel (0, 0), rows of 256 bytes
//   8     texture: [8:0] width, [24:16] height (1..256), [31] texel 0 see-through
//   9     reference pixel: [15:0] x, [31:16] y
//   10..12  Q, U, V at the reference pixel's centre (signed)
//   13..15  their change per pixel in x, 8 fraction bits    16..18  the same in y
//   19    write: forget the cached texture lines (after the CPU changed textures)
//   20    texture lines fetched from the SDRAM (cache misses); write: clear
//   21    clocks the rasterizer was busy; write: clear (and the counters below)
// Counters (outputs, cleared with 21): clocks the texture pipeline waited for a line, in
// edge setup, in the work of a row (rows, span, the values at its first pixel), and idle
// with the setup unit's next triangle waiting for the texture pipeline.
//
// The setup unit's triangle is taken while the texture pipeline still draws the one before:
// a textured one into the same buffer once the pixels have passed its front (the queue
// keeps what they need), anything else once it is empty.
//

module raster (
    input   wire            clk,
    input   wire            reset,              // abandons the triangle being drawn

    input   wire            wr,
    input   wire    [4:0]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            busy,
    output  reg     [31:0]  pixels = 0,         // pixels drawn since the last clear
    output  wire    [31:0]  misses,             // texture cache misses since the last clear
    output  reg     [31:0]  busy_clocks = 0,    // clocks with busy since the last clear
    output  reg     [31:0]  miss_clocks = 0,
    output  reg     [31:0]  edge_clocks = 0,
    output  reg     [31:0]  row_clocks = 0,
    output  reg     [31:0]  drain_clocks = 0,

    // framebuffer drawing port, a word of four pixels; be[3] is the leftmost pixel
    output  wire            fb_wr,
    output  reg             fb_buf,
    output  wire    [13:0]  fb_addr,
    output  wire    [31:0]  fb_data,
    output  wire    [3:0]   fb_be,

    // texture lines from the SDRAM (sdram_port)
    output  wire            mem_valid,
    input   wire            mem_ready,
    output  wire    [25:5]  mem_addr,
    input   wire            mem_rsp_valid,
    input   wire    [31:0]  mem_rsp_data,

    // a triangle from the setup unit (setup.v): taken instead of the registers 0..18 and the
    // draw word in a clock with su_go and su_accept
    input   wire            su_go,
    output  wire            su_accept,
    input   wire    [31:0]  su_v0, su_v1, su_v2, su_ref, su_tex, su_size,
    input   wire    [9:0]   su_draw,
    input   wire    [31:0]  su_a0, su_a1, su_a2,
    input   wire    [31:0]  su_dx0, su_dx1, su_dx2,
    input   wire    [31:0]  su_dy0, su_dy1, su_dy2
);

    localparam  [3:0]   S_IDLE = 4'd0;
    localparam  [3:0]   S_ROWS = 4'd1;          // first, middle and last row
    localparam  [3:0]   S_EDGE = 4'd2;          // edge setup (div, mul, add)
    localparam  [3:0]   S_ROW  = 4'd3;          // a row: both edges at its top and bottom
    localparam  [3:0]   S_ROW2 = 4'd10;         // each edge's first and end pixel
    localparam  [3:0]   S_CLIP = 4'd4;          // limited to the clip rectangle
    localparam  [3:0]   S_SPAN = 4'd5;          // its words, or the next row if empty
    localparam  [3:0]   S_FILL = 4'd6;          // the words of one colour
    localparam  [3:0]   S_ACC  = 4'd7;          // textured: Q, U, V at the span's first pixel
    localparam  [3:0]   S_TEX  = 4'd8;          // textured: the pixels into raster_tex
    localparam  [3:0]   S_GO   = 4'd9;          // nothing to draw, or the first edge

    reg  [3:0]  state = S_IDLE;
    reg         flat_wr = 0;                    // one-colour words
    reg  [13:0] flat_addr;
    reg  [3:0]  flat_be;
    wire        tex_busy, tex_front, tex_wr, tex_ready, tex_stalled;
    wire [13:0] tex_word;
    wire [3:0]  tex_be;
    wire [7:0]  tex_texel;
    assign busy = state != S_IDLE || flat_wr || tex_busy;

//
// registers the CPU writes
//
    reg  [31:0] v_reg [0:2];
    reg  [31:0] clip0, clip1, tex_reg, size_reg, ref_reg;
    reg  [31:0] a_reg [0:2];
    reg  [31:0] dx_reg [0:2];
    reg  [31:0] dy_reg [0:2];
    wire        start = wr && wr_addr == 5'd5 && state == S_IDLE && !reset;
    assign su_accept = state == S_IDLE && !flat_wr && !reset &&
                       (su_draw[9] && su_draw[8] == fb_buf ? !tex_front : !tex_busy);
    wire        su_take = su_go && su_accept;           // the setup unit's triangle
    wire [4:0]  wr_a5 = wr_addr - 5'd10, wr_dx5 = wr_addr - 5'd13, wr_dy5 = wr_addr - 5'd16;
    wire [1:0]  wr_addr_a = wr_a5[1:0], wr_addr_dx = wr_dx5[1:0], wr_addr_dy = wr_dy5[1:0];

always @(posedge clk) begin
    if(wr && wr_addr <= 5'd2) v_reg[wr_addr[1:0]] <= wr_data;
    if(wr && wr_addr == 5'd3) clip0 <= wr_data;
    if(wr && wr_addr == 5'd4) clip1 <= wr_data;
    if(wr && wr_addr == 5'd7) tex_reg <= wr_data;
    if(wr && wr_addr == 5'd8) size_reg <= wr_data;
    if(wr && wr_addr == 5'd9) ref_reg <= wr_data;
    if(wr && wr_addr >= 5'd10 && wr_addr <= 5'd12) a_reg[wr_addr_a] <= wr_data;
    if(wr && wr_addr >= 5'd13 && wr_addr <= 5'd15) dx_reg[wr_addr_dx] <= wr_data;
    if(wr && wr_addr >= 5'd16 && wr_addr <= 5'd18) dy_reg[wr_addr_dy] <= wr_data;
end

    // sorted by y with the swaps of the C model
    reg signed [15:0] sx [0:2];
    reg signed [15:0] sy [0:2];
    reg signed [15:0] tx, ty;
    // the values come from the setup unit while it offers a triangle (su_go, a register:
    // the CPU starts none then, soc.v holds its draw command back)
    wire [31:0] v_in0 = su_go ? su_v0 : v_reg[0];
    wire [31:0] v_in1 = su_go ? su_v1 : v_reg[1];
    wire [31:0] v_in2 = su_go ? su_v2 : v_reg[2];
always @(*) begin
    tx = 0;
    ty = 0;
    sx[0] = v_in0[15:0]; sy[0] = v_in0[31:16];
    sx[1] = v_in1[15:0]; sy[1] = v_in1[31:16];
    sx[2] = v_in2[15:0]; sy[2] = v_in2[31:16];
    if(sy[1] < sy[0]) begin tx = sx[0]; ty = sy[0]; sx[0] = sx[1]; sy[0] = sy[1]; sx[1] = tx; sy[1] = ty; end
    if(sy[2] < sy[1]) begin tx = sx[1]; ty = sy[1]; sx[1] = sx[2]; sy[1] = sy[2]; sx[2] = tx; sy[2] = ty; end
    if(sy[1] < sy[0]) begin tx = sx[0]; ty = sy[0]; sx[0] = sx[1]; sy[0] = sy[1]; sx[1] = tx; sy[1] = ty; end
end

    function signed [15:0] limit(input signed [15:0] v, input signed [15:0] hi);
        limit = v < 0 ? 16'sd0 : v > hi ? hi : v;
    endfunction

    // a corner cut to a whole pixel (1/16 pixel units), and its pixel
    function signed [15:0] whole(input signed [15:0] v);
        whole = {v[15:4], 4'd0};
    endfunction
    function signed [15:0] pix(input signed [15:0] v);
        pix = {{4{v[15]}}, v[15:4]};
    endfunction
    function signed [15:0] min16(input signed [15:0] p, input signed [15:0] q);
        min16 = p < q ? p : q;
    endfunction
    function signed [15:0] max16(input signed [15:0] p, input signed [15:0] q);
        max16 = p > q ? p : q;
    endfunction

//
// the triangle being drawn
//
    reg signed [15:0] ax, ay, bx, by, cx, cy;
    reg  [7:0]  colour;
    reg signed [15:0] cx0, cy0, cx1, cy1;       // clip, within the screen
    reg signed [15:0] r, rb, r1, r0, rc;        // rows: b's, end (exclusive), first, c's
    reg signed [15:0] last0, last1;             // row rc: the corners on it, first and last pixel
    reg  [1:0]  job;                            // edge being set up: 0 long, 1 short, 2 b-c later
    reg signed [35:0] l_x, s_x, s2_x;           // 16.16 pixels
    reg signed [32:0] l_dx, s_dx, s2_dx;        // slopes
    reg  [13:0] row_base;                       // r * 80
    reg  [9:0]  px0, px1;                       // span [px0, px1)
    reg  [7:0]  w, w1;                          // framebuffer words of the span, w1 inclusive

    // textured
    reg         textured;
    reg  [25:0] t_base;
    reg  [8:0]  t_w, t_h;
    reg         t_cut;
    reg signed [15:0] t_xr, t_yr;
    reg signed [31:0] t_a [0:2];
    reg signed [31:0] t_dx [0:2];
    reg signed [31:0] t_dy [0:2];
    reg signed [43:0] acc [0:2];                // Q, U, V at pixel x, 8 fraction bits
    reg signed [42:0] acc_px, acc_py;           // dx (x0 - xr), dy (r - yr)
    reg  [1:0]  acc_k;
    reg  [9:0]  x;                              // textured: the pixel going in

    wire signed [10:0] acc_dx = $signed({1'b0, px0}) - $signed(t_xr[10:0]);
    wire signed [10:0] acc_dy = r[10:0] - t_yr[10:0];

    assign fb_wr = flat_wr || tex_wr;
    assign fb_addr = tex_wr ? tex_word : flat_addr;
    assign fb_be = tex_wr ? tex_be : flat_be;
    assign fb_data = tex_wr ? {4{tex_texel}} : {4{colour}};

raster_tex rt (
    .clk                ( clk ),
    .reset              ( reset ),
    .invalidate         ( wr && wr_addr == 5'd19 ),
    .in_valid           ( state == S_TEX ),
    .in_ready           ( tex_ready ),
    .in_q               ( acc[0] ),
    .in_u               ( acc[1] ),
    .in_v               ( acc[2] ),
    .in_word            ( row_base + {6'd0, x[9:2]} ),
    .in_lane            ( x[1:0] ),
    .tex_base           ( t_base ),
    .tex_w              ( t_w ),
    .tex_h              ( t_h ),
    .tex_cut            ( t_cut ),
    .busy               ( tex_busy ),
    .front_busy         ( tex_front ),
    .misses             ( misses ),
    .stalled            ( tex_stalled ),
    .clear_misses       ( wr && wr_addr == 5'd20 ),
    .out_wr             ( tex_wr ),
    .out_word           ( tex_word ),
    .out_be             ( tex_be ),
    .out_texel          ( tex_texel ),
    .mem_valid          ( mem_valid ),
    .mem_ready          ( mem_ready ),
    .mem_addr           ( mem_addr ),
    .mem_rsp_valid      ( mem_rsp_valid ),
    .mem_rsp_data       ( mem_rsp_data )
);

//
// edge setup: slope = ((x1 - x0) << 16) / (y1 - y0), x = (x0 << 12) + (slope * dyc >>> 4)
//
    reg signed [15:0] e_x0, e_y0, e_x1, e_y1, e_r;
    reg  [1:0]  e_phase;                        // 0 divide, 1 sign, 2 multiply, 3 add
    reg  [5:0]  e_cnt;
    reg         e_neg;
    reg  [31:0] e_num;                          // |x1 - x0| << 16, shifted out at the top
    reg  [15:0] e_den;
    reg  [16:0] e_rem;
    reg  [31:0] e_quo;
    reg signed [32:0] e_slope;
    reg signed [52:0] e_prod;
    // two steps of the restoring division a clock: the first bit, then the second from it
    wire [16:0] e_try = {e_rem[15:0], e_num[31]};
    wire        e_q1 = e_try >= {1'b0, e_den};
    wire [16:0] e_rem1 = e_q1 ? e_try - {1'b0, e_den} : e_try;
    wire [16:0] e_try2 = {e_rem1[15:0], e_num[30]};
    wire        e_q2 = e_try2 >= {1'b0, e_den};
    wire [16:0] e_rem2 = e_q2 ? e_try2 - {1'b0, e_den} : e_try2;
    wire signed [19:0] e_dyc = $signed({e_r, 4'd0}) - $signed({{4{e_y0[15]}}, e_y0});    // 16 r - y0, >= 0
    wire signed [35:0] e_x = $signed({{8{e_x0[15]}}, e_x0, 12'd0}) + $signed(e_prod[39:4]);

    task edge_start(input signed [15:0] x0, input signed [15:0] y0, input signed [15:0] x1,
                    input signed [15:0] y1, input signed [15:0] row);
        reg signed [16:0] dx;
        begin
            dx = {x1[15], x1} - {x0[15], x0};
            e_x0 <= x0; e_y0 <= y0; e_x1 <= x1; e_y1 <= y1; e_r <= row;
            e_neg <= dx < 0;
            e_num <= {(dx < 0 ? -dx[15:0] : dx[15:0]), 16'd0};
            e_den <= y1 - y0;
            e_rem <= 0;
            e_cnt <= 0;
            e_phase <= 0;
        end
    endtask

//
// a row's span from the two edges, over four clocks (S_ROW, S_ROW2, S_CLIP, S_SPAN)
//
    // a row (S_ROW): each edge at its top (t) and bottom (b), the corner's x at its lower end
    reg signed [35:0] lt, lb, st, sb;
    wire signed [35:0] bx16 = {{8{bx[15]}}, bx, 12'd0}, cx16 = {{8{cx[15]}}, cx, 12'd0};
    reg         row_last;                       // row rc
    // (S_ROW2): each edge's first pixel min(ceil t, floor b + 1) and end max(floor t + 1, ceil b)
    wire signed [35:0] lt_c = lt + 36'sh0FFFF, lb_c = lb + 36'sh0FFFF;
    wire signed [35:0] st_c = st + 36'sh0FFFF, sb_c = sb + 36'sh0FFFF;
    wire signed [19:0] l_lo_a = lt_c[35:16], l_lo_b = lb[35:16] + 20'sd1;
    wire signed [19:0] l_hi_a = lt[35:16] + 20'sd1, l_hi_b = lb_c[35:16];
    wire signed [19:0] s_lo_a = st_c[35:16], s_lo_b = sb[35:16] + 20'sd1;
    wire signed [19:0] s_hi_a = st[35:16] + 20'sd1, s_hi_b = sb_c[35:16];
    reg signed [19:0] l_lo, l_hi, s_lo, s_hi;
    reg         long_left;
    reg         row_last2;
    // (S_CLIP): the span from the left edge's first pixel to the right edge's end
    wire signed [19:0] col_l = row_last2 ? {{4{last0[15]}}, last0} : long_left ? l_lo : s_lo;
    wire signed [19:0] col_r = row_last2 ? {{4{last1[15]}}, last1} + 20'sd1 : long_left ? s_hi : l_hi;
    wire signed [19:0] clip_l = {{4{cx0[15]}}, cx0}, clip_r = {{4{cx1[15]}}, cx1};
    wire [9:0]  span_l = col_l < clip_l ? cx0[9:0] : col_l > clip_r ? cx1[9:0] : col_l[9:0];
    wire [9:0]  span_r = col_r < clip_l ? cx0[9:0] : col_r > clip_r ? cx1[9:0] : col_r[9:0];
    wire [9:0]  px_last = px1 - 1'b1;

    // byte enables of word w: pixel 4w + k is drawn when px0 <= 4w + k < px1
    reg  [3:0]  be;
    integer     k, j;
always @(*) begin
    be = 0;
    for(k = 0; k < 4; k = k + 1)
        if({w, k[1:0]} >= px0 && {w, k[1:0]} < px1) be[3 - k] = 1;
end

always @(posedge clk) begin
    flat_wr <= 0;
    if(wr && wr_addr == 5'd21) begin
        busy_clocks <= 0;
        miss_clocks <= 0;
        edge_clocks <= 0;
        row_clocks <= 0;
        drain_clocks <= 0;
    end else begin
        if(busy) busy_clocks <= busy_clocks + 1'b1;
        if(tex_stalled && tex_busy) miss_clocks <= miss_clocks + 1'b1;
        if(state == S_EDGE) edge_clocks <= edge_clocks + 1'b1;
        if(state == S_ROWS || state == S_GO || state == S_ROW || state == S_ROW2 || state == S_CLIP ||
           state == S_SPAN || state == S_ACC)
            row_clocks <= row_clocks + 1'b1;
        if(state == S_IDLE && su_go && !su_accept) drain_clocks <= drain_clocks + 1'b1;
    end
    if(wr && wr_addr == 5'd6) pixels <= 0;
    else if(fb_wr) pixels <= pixels + fb_be[0] + fb_be[1] + fb_be[2] + fb_be[3];

    case(state)
    S_IDLE: if(start || su_take) begin
        ax <= whole(sx[0]); ay <= whole(sy[0]);
        bx <= whole(sx[1]); by <= whole(sy[1]);
        cx <= whole(sx[2]); cy <= whole(sy[2]);
        if(su_go) begin
            colour <= su_draw[7:0];
            fb_buf <= su_draw[8];
            textured <= su_draw[9];
            t_base <= su_tex[25:0];
            t_w <= su_size[8:0];
            t_h <= su_size[24:16];
            t_cut <= su_size[31];
            t_xr <= su_ref[15:0];
            t_yr <= su_ref[31:16];
            t_a[0] <= su_a0; t_a[1] <= su_a1; t_a[2] <= su_a2;
            t_dx[0] <= su_dx0; t_dx[1] <= su_dx1; t_dx[2] <= su_dx2;
            t_dy[0] <= su_dy0; t_dy[1] <= su_dy1; t_dy[2] <= su_dy2;
        end else begin
            colour <= wr_data[7:0];
            fb_buf <= wr_data[8];
            textured <= wr_data[9];
            t_base <= tex_reg[25:0];
            t_w <= size_reg[8:0];
            t_h <= size_reg[24:16];
            t_cut <= size_reg[31];
            t_xr <= ref_reg[15:0];
            t_yr <= ref_reg[31:16];
            for(j = 0; j < 3; j = j + 1) begin
                t_a[j] <= a_reg[j];
                t_dx[j] <= dx_reg[j];
                t_dy[j] <= dy_reg[j];
            end
        end
        cx0 <= limit(clip0[15:0], 16'sd320);
        cy0 <= limit(clip0[31:16], 16'sd200);
        cx1 <= limit(clip1[15:0], 16'sd320);
        cy1 <= limit(clip1[31:16], 16'sd200);
        state <= S_ROWS;
    end

    S_ROWS: begin
        // r0 = max(a's row, clip y0), r1 = min(c's row + 1, clip y1)
        r0 <= pix(ay) < cy0 ? cy0 : pix(ay);
        r  <= pix(ay) < cy0 ? cy0 : pix(ay);
        rb <= pix(by);
        rc <= pix(cy);
        r1 <= pix(cy) + 16'sd1 > cy1 ? cy1 : pix(cy) + 16'sd1;
        last0 <= by != cy ? pix(cx) : ay != cy ? min16(pix(bx), pix(cx)) : min16(pix(ax), min16(pix(bx), pix(cx)));
        last1 <= by != cy ? pix(cx) : ay != cy ? max16(pix(bx), pix(cx)) : max16(pix(ax), max16(pix(bx), pix(cx)));
        state <= S_GO;
    end

    S_GO: begin                                 // (a clock of its own: the rows are registers)
        row_base <= {r0[7:0], 6'd0} + {2'd0, r0[7:0], 4'd0};
        if(r0 >= r1) state <= S_IDLE;           // nothing to draw
        else if(r0 >= rc) state <= S_ROW;       // row c.y alone: no edge needed
        else begin
            edge_start(ax, ay, cx, cy, r0);
            job <= 0;
            state <= S_EDGE;
        end
    end

    S_EDGE: begin
        case(e_phase)
        2'd0: begin                             // two quotient bits a clock
            e_num <= {e_num[29:0], 2'b00};
            e_rem <= e_rem2;
            e_quo <= {e_quo[29:0], e_q1, e_q2};
            e_cnt <= e_cnt + 1'b1;
            if(e_cnt == 6'd15) e_phase <= 1;
        end
        2'd1: begin
            e_slope <= e_neg ? -$signed({1'b0, e_quo}) : $signed({1'b0, e_quo});
            e_phase <= 2;
        end
        2'd2: begin
            e_prod <= e_slope * e_dyc;
            e_phase <= 3;
        end
        default: begin
            case(job)
            2'd0: begin l_x <= e_x; l_dx <= e_slope; end
            2'd1: begin s_x <= e_x; s_dx <= e_slope; end
            default: begin s2_x <= e_x; s2_dx <= e_slope; end
            endcase
            if(job == 2'd0) begin
                job <= 1;
                if(r0 < rb) edge_start(ax, ay, bx, by, r0);
                else edge_start(bx, by, cx, cy, r0);
            end else if(job == 2'd1 && r0 < rb && rb < rc && rb < r1) begin
                job <= 2;
                edge_start(bx, by, cx, cy, rb);
            end else
                state <= S_ROW;
        end
        endcase
    end

    S_ROW: begin
        lt <= l_x;
        st <= s_x;
        lb <= r + 16'sd1 == rc ? cx16 : l_x + {{3{l_dx[32]}}, l_dx};
        sb <= r + 16'sd1 == rb ? bx16 : r + 16'sd1 == rc ? cx16 : s_x + {{3{s_dx[32]}}, s_dx};
        row_last <= r == rc;
        state <= S_ROW2;
    end

    S_ROW2: begin
        l_lo <= l_lo_a < l_lo_b ? l_lo_a : l_lo_b;
        l_hi <= l_hi_a > l_hi_b ? l_hi_a : l_hi_b;
        s_lo <= s_lo_a < s_lo_b ? s_lo_a : s_lo_b;
        s_hi <= s_hi_a > s_hi_b ? s_hi_a : s_hi_b;
        long_left <= lt < st || (lt == st && lb < sb);
        row_last2 <= row_last;
        state <= S_CLIP;
    end

    S_CLIP: begin
        px0 <= span_l;
        px1 <= span_r;
        state <= S_SPAN;
    end

    S_SPAN: begin
        w <= px0[9:2];
        w1 <= px_last[9:2];
        x <= px0;
        acc_k <= 0;
        if(px0 >= px1) next_row;
        else state <= textured ? S_ACC : S_FILL;
    end

    S_FILL: begin
        flat_wr <= 1;
        flat_addr <= row_base + {6'd0, w};
        flat_be <= be;
        w <= w + 1'b1;
        if(w == w1) next_row;
    end

    S_ACC: begin                                // products for k, the sum one clock later
        acc_k <= acc_k + 1'b1;
        if(acc_k != 2'd3) begin
            acc_px <= t_dx[acc_k] * acc_dx;
            acc_py <= t_dy[acc_k] * acc_dy;
        end
        if(acc_k != 2'd0)
            acc[acc_k - 2'd1] <= $signed({{4{t_a[acc_k - 2'd1][31]}}, t_a[acc_k - 2'd1], 8'd0}) +
                                 $signed({acc_px[42], acc_px}) + $signed({acc_py[42], acc_py});
        if(acc_k == 2'd3) state <= S_TEX;
    end

    S_TEX: if(tex_ready) begin
        x <= x + 1'b1;
        for(j = 0; j < 3; j = j + 1) acc[j] <= acc[j] + {{12{t_dx[j][31]}}, t_dx[j]};
        if(x == px_last) next_row;
    end

    default: state <= S_IDLE;
    endcase

    if(reset) state <= S_IDLE;
end

    // to the next row: both edges one row on; from row rb on the edge b-c (not needed when
    // rb is c's row: that row takes its corners)
    task next_row;
        begin
            r <= r + 16'sd1;
            row_base <= row_base + 14'd80;
            l_x <= l_x + {{3{l_dx[32]}}, l_dx};
            if(r + 16'sd1 == rb) begin
                s_x <= s2_x;
                s_dx <= s2_dx;
            end else begin
                s_x <= s_x + {{3{s_dx[32]}}, s_dx};
            end
            state <= r + 16'sd1 == r1 ? S_IDLE : S_ROW;
        end
    endtask

endmodule
