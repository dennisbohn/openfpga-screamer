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
// Widths: the corners are kept in whole pixels (12 bits). In the rows above c.y, the only
// ones an edge's x is used in, 0 <= r - y0 < dy, so x lies between the edge's ends,
// |x| <= 2^27 (16.16): 28 bits hold every x exactly, and as x only meets the slope in sums
// that end up there, 28 bits of the slope (|slope| < 2^28) do as well. The rows drawn are
// those inside the clip rectangle, 0..199.
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
    localparam  [3:0]   S_ROW  = 4'd3;          // a row: which edge is left, which ends to take
    localparam  [3:0]   S_ROW2 = 4'd10;         // the span's start, limited to the clip rectangle
    localparam  [3:0]   S_CLIP = 4'd4;          // its end, the same
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
// registers the CPU writes: 0..4 in registers (a triangle starts with them), 7..18 in a RAM
// of two banks, twice (two reads a clock). Per register, l_bank is the bank its latest value
// is in and t_bank the one the triangle last started reads; a write goes to the other bank
// than t_bank. A triangle from the CPU (soc.v starts it only when the rasterizer is idle)
// takes 7..18 in the six clocks after its start, two a clock, by step ld (dx, dy and a each
// shifted in at place 2); the first values are used at the earliest six clocks after the
// start (S_ACC), the texture's in raster_tex later still.
//
    reg  [31:0] v_reg [0:2];
    reg  [8:0]  clip_x0, clip_x1;               // 3 and 4, limited to the screen
    reg  [7:0]  clip_y0, clip_y1;
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] cr_a [0:63];
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] cr_b [0:63];
    reg  [31:0] cr_qa, cr_qb;
    reg  [31:0] l_bank = 0, t_bank = 0;         // bits 7..18
    reg  [2:0]  ld = 0;                         // step + 1 while the values come in
    wire        start = wr && wr_addr == 5'd5 && state == S_IDLE && !reset;
    assign su_accept = state == S_IDLE && !flat_wr && !reset &&
                       (su_draw[9] && su_draw[8] == fb_buf ? !tex_front : !tex_busy);
    wire        su_take = su_go && su_accept;           // the setup unit's triangle
    wire        cr_wr = wr && wr_addr >= 5'd7 && wr_addr <= 5'd18;
    // v limited to [0, hi]
    function [8:0] limit(input signed [15:0] v, input [8:0] hi);
        limit = v < 0 ? 9'd0 : v > $signed({7'd0, hi}) ? hi : v[8:0];
    endfunction
    wire [8:0]  wr_cy = limit(wr_data[31:16], 9'd200);
    // the two registers read for each step (at the start's clock edge step 0, then ld)
    wire [2:0]  ld_step = state == S_IDLE ? 3'd0 : ld;
    reg  [4:0]  ra, rb;
always @(*) begin
    case(ld_step)
    3'd0:    begin ra = 5'd9;  rb = 5'd13; end  // reference pixel, dx Q
    3'd1:    begin ra = 5'd16; rb = 5'd14; end  // dy Q, dx U
    3'd2:    begin ra = 5'd17; rb = 5'd15; end  // dy U, dx V
    3'd3:    begin ra = 5'd18; rb = 5'd10; end  // dy V, Q
    3'd4:    begin ra = 5'd7;  rb = 5'd11; end  // texture address, U
    default: begin ra = 5'd8;  rb = 5'd12; end  // texture size, V
    endcase
end
    wire        ra_bank = state == S_IDLE ? l_bank[ra] : t_bank[ra];
    wire        rb_bank = state == S_IDLE ? l_bank[rb] : t_bank[rb];

always @(posedge clk) begin
    if(wr && wr_addr <= 5'd2) v_reg[wr_addr[1:0]] <= wr_data;
    if(wr && wr_addr == 5'd3) begin clip_x0 <= limit(wr_data[15:0], 9'd320); clip_y0 <= wr_cy[7:0]; end
    if(wr && wr_addr == 5'd4) begin clip_x1 <= limit(wr_data[15:0], 9'd320); clip_y1 <= wr_cy[7:0]; end
    if(cr_wr) begin
        cr_a[{!t_bank[wr_addr], wr_addr}] <= wr_data;
        cr_b[{!t_bank[wr_addr], wr_addr}] <= wr_data;
        l_bank[wr_addr] <= !t_bank[wr_addr];
    end
    cr_qa <= cr_a[{ra_bank, ra}];
    cr_qb <= cr_b[{rb_bank, rb}];
    if(start && !su_go) begin
        t_bank <= l_bank;
        ld <= 3'd1;
    end else if(ld != 3'd0 && ld != 3'd6 && state != S_IDLE)
        ld <= ld + 1'b1;
    else
        ld <= 3'd0;
end

    // sorted by y as the swaps of the C model do: a stable sort, so each vertex's place
    // follows from three comparisons at once (v0: c01 + c02, v1: !c01 + c12, v2: !c02 + !c12);
    // the values come from the setup unit while it offers a triangle (su_go, a register:
    // the CPU starts none then, soc.v holds its draw command back)
    wire [31:0] v_in0 = su_go ? su_v0 : v_reg[0];
    wire [31:0] v_in1 = su_go ? su_v1 : v_reg[1];
    wire [31:0] v_in2 = su_go ? su_v2 : v_reg[2];
    wire        c01 = $signed(v_in1[31:16]) < $signed(v_in0[31:16]);
    wire        c12 = $signed(v_in2[31:16]) < $signed(v_in1[31:16]);
    wire        c02 = $signed(v_in2[31:16]) < $signed(v_in0[31:16]);
    // {y, x} in whole pixels
    wire [23:0] p0 = {v_in0[31:20], v_in0[15:4]}, p1 = {v_in1[31:20], v_in1[15:4]}, p2 = {v_in2[31:20], v_in2[15:4]};
    wire [23:0] pa = !c01 && !c02 ? p0 : c01 && !c12 ? p1 : p2;
    wire [23:0] pb = c01 != c02 ? p0 : c01 == c12 ? p1 : p2;
    wire [23:0] pc = c01 && c02 ? p0 : !c01 && c12 ? p1 : p2;

//
// the triangle being drawn
//
    reg signed [11:0] ax, ay, bx, by, cx, cy;   // corners in whole pixels
    reg  [7:0]  colour;
    reg  [8:0]  cx0, cx1;                       // clip, within the screen
    reg  [7:0]  cy0, cy1;
    reg  [7:0]  r;                              // row being drawn
    reg         go_none, go_alone;              // nothing to draw, row c.y alone
    reg signed [11:0] last0, last1;             // row c.y: the corners on it, first and last pixel
    reg  [1:0]  job;                            // edge being set up: 0 long, 1 short, 2 b-c later
    reg signed [27:0] l_x, s_x;                 // 16.16 pixels, at the top of row r
    reg  [27:0] l_dx, s_dx;                     // slopes, the low 28 bits
    reg  [8:0]  px0, px1;                       // span [px0, px1)
    reg  [6:0]  w1;                             // the span's last framebuffer word
    reg  [8:0]  x;                              // the pixel going in (textured), its word x[8:2]
    reg         first;                          // S_FILL: the span's first word

    // textured
    reg         textured;
    reg  [25:0] t_base;
    reg  [8:0]  t_w, t_h;
    reg         t_cut;
    reg  [10:0] t_xr, t_yr;
    reg signed [31:0] t_a [0:2];
    reg signed [31:0] t_dx [0:2];
    reg signed [31:0] t_dy [0:2];
    reg signed [43:0] acc [0:2];                // Q, U, V at pixel x, 8 fraction bits
    reg signed [42:0] acc_px, acc_py;           // dx (x0 - xr), dy (r - yr)
    reg  [1:0]  acc_k;

    wire signed [10:0] acc_dx = {2'd0, px0} - t_xr;
    wire signed [10:0] acc_dy = {3'd0, r} - t_yr;
    // the framebuffer word of pixel x in row r: 80 r + x / 4
    wire [13:0] x_word = {r, 6'd0} + {2'd0, r, 4'd0} + {7'd0, x[8:2]};
    wire [8:0]  x_next = x + {6'd0, state == S_FILL, 1'b0, state != S_FILL};    // a word on, or a pixel

    // row r against the corners' rows and the clip
    wire signed [11:0] r_s = {4'd0, r}, r1_s = {3'd0, r + 9'd1};
    wire        r_is_c = r_s == cy;             // the last row: its corners
    wire        r1_is_b = r1_s == by, r1_is_c = r1_s == cy;
    wire        r_end = r_is_c || r1_s == {4'd0, cy1};

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
    .in_word            ( x_word ),
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
// rows (S_ROWS): r0 = max(a's row, clip y0); rows r0 .. min(c's row + 1, clip y1) - 1
//
    wire signed [11:0] r0_c = ay < $signed({4'd0, cy0}) ? $signed({4'd0, cy0}) : ay;
    wire        bc_lt = bx < cx;
    wire signed [11:0] bc_min = bc_lt ? bx : cx, bc_max = bc_lt ? cx : bx;

//
// edge setup in whole pixels: slope = (|dx| << 16) / dy, x = (x0 << 16) +- slope (r - y0)
//
    reg  [1:0]  e_phase;                        // 0 divide, 1 wait, 2 multiply, 3 add
    reg  [3:0]  e_cnt;
    reg         e_neg;
    reg signed [11:0] e_x0;
    reg  [11:0] e_den;                          // dy >= 1
    reg  [11:0] e_dy;                           // r - y0, below dy
    reg  [11:0] e_rem;
    reg  [27:0] e_q;                            // |dx| << 16 shifted out at the top, the quotient in
    reg  [27:0] e_prod;                         // |slope| (r - y0) < 2^28
    // two steps of the restoring division a clock (14 clocks, then two more as before): the
    // first bit, then the second from it; a bit is 1 when the subtraction does not borrow
    wire [12:0] e_try1 = {e_rem, e_q[27]};
    wire [13:0] e_sub1 = {1'b0, e_try1} - {2'b00, e_den};
    wire [11:0] e_rem1 = e_sub1[13] ? e_try1[11:0] : e_sub1[11:0];
    wire [12:0] e_try2 = {e_rem1, e_q[26]};
    wire [13:0] e_sub2 = {1'b0, e_try2} - {2'b00, e_den};
    wire [11:0] e_rem2 = e_sub2[13] ? e_try2[11:0] : e_sub2[11:0];
    // the edge's results; the last edge's stay here (b-c from row b.y on)
    wire [27:0] e_slope = e_neg ? -e_q : e_q;
    wire signed [27:0] e_x = {e_x0, 16'd0} + (e_neg ? -e_prod : e_prod);

    // the edge edge_start sets up, by where it is called: a-c in S_GO, then in S_EDGE a-b
    // while the first row is above b's, else b-c (from the first row, or from b.y for job 1)
    wire        n_ab = state == S_EDGE && job == 2'd0 && r_s < by;
    wire        n_bc = state == S_EDGE && !n_ab;
    wire signed [11:0] n_x0 = n_bc ? bx : ax, n_y0 = n_bc ? by : ay;
    wire signed [11:0] n_x1 = n_ab ? bx : cx, n_y1 = n_ab ? by : cy;
    wire signed [12:0] n_dx = {n_x1[11], n_x1} - {n_x0[11], n_x0};

    task edge_start;
        begin
            e_x0 <= n_x0;
            e_neg <= n_dx[12];
            e_q <= {(n_dx[12] ? -n_dx[11:0] : n_dx[11:0]), 16'd0};
            e_den <= n_y1 - n_y0;
            e_dy <= state == S_EDGE && job == 2'd1 ? 12'd0 : r_s - n_y0;
            e_rem <= 0;
            e_cnt <= 0;
            e_phase <= 0;
        end
    endtask

//
// a row's span from the two edges, over four clocks (S_ROW, S_ROW2, S_CLIP, S_SPAN)
//
    // each edge at the top of the row (t) is l_x, s_x, at its bottom (b) x + slope, the
    // corner's x at its lower end; both stay as they are until the row's span is done
    wire signed [27:0] bx16 = {bx, 16'd0}, cx16 = {cx, 16'd0};
    wire signed [27:0] lb = r1_is_c ? cx16 : l_x + l_dx;
    wire signed [27:0] sb = r1_is_b ? bx16 : r1_is_c ? cx16 : s_x + s_dx;
    // (S_ROW): the left edge, and for each edge whether its first pixel min(ceil t, floor b + 1)
    // and its end max(floor t + 1, ceil b) come from t. With T, B the integer parts and ft, fb
    // set when there is a fraction: ceil t <= floor b + 1 when B - T >= ft - 1, floor t + 1 >=
    // ceil b when B - T <= 1 - fb
    wire signed [12:0] l_t = {l_x[27], l_x[27:16]}, l_b = {lb[27], lb[27:16]};
    wire signed [12:0] s_t = {s_x[27], s_x[27:16]}, s_b = {sb[27], sb[27:16]};
    wire        l_ft = |l_x[15:0], l_fb = |lb[15:0], s_ft = |s_x[15:0], s_fb = |sb[15:0];
    wire signed [12:0] l_d = l_b - l_t, s_d = s_b - s_t;
    reg         long_left, l_lo_t, l_hi_t, s_lo_t, s_hi_t;
    // the span's ends one after the other through the same adder and clip, the left edge's
    // first pixel in S_ROW2 (px0), the right edge's end in S_CLIP (px1); row c.y: its corners
    wire        e_left = state == S_ROW2;
    wire        e_l = e_left == long_left;      // the end is the long edge's
    wire        e_use_t = e_left ? (e_l ? l_lo_t : s_lo_t) : (e_l ? l_hi_t : s_hi_t);
    wire signed [12:0] e_base = r_is_c ? (e_left ? {last0[11], last0} : {last1[11], last1}) :
                                e_use_t ? (e_l ? l_t : s_t) : (e_l ? l_b : s_b);
    wire        e_inc = r_is_c ? !e_left :
                        e_left ? (!e_use_t || (e_l ? l_ft : s_ft)) : (e_use_t || (e_l ? l_fb : s_fb));
    wire signed [12:0] e_col = e_base + {12'd0, e_inc};
    wire [8:0]  e_end = e_col < $signed({4'd0, cx0}) ? cx0 : e_col > $signed({4'd0, cx1}) ? cx1 : e_col[8:0];
    wire [8:0]  px_last = px1 - 1'b1;

    // byte enables of word w = x[8:2] (S_FILL, px0[8:2] .. w1): pixel 4w + k is drawn when
    // px0 <= 4w + k < px1, so k >= px0[1:0] in the first word and k <= px_last[1:0] in the last
    wire [3:0]  be = (first ? 4'b1111 >> px0[1:0] : 4'b1111) &
                     (x[8:2] == w1 ? ~(4'b0111 >> px_last[1:0]) : 4'b1111);

    // textured: the values of a triangle from the CPU come in at place 2 (dx at steps 1..3,
    // dy at 2..4, a at 4..6); S_ACC rotates them (k = 0, 1, 2)
    wire        dx_in = ld >= 3'd1 && ld <= 3'd3, dy_in = ld >= 3'd2 && ld <= 3'd4, a_in = ld >= 3'd4;
    wire        acc_rot = state == S_ACC && acc_k != 2'd3;

    // edge, row and drain clocks are counted in different states: one adder for the three
    wire        c_edge = state == S_EDGE;
    wire        c_row = state == S_ROWS || state == S_GO || state == S_ROW || state == S_ROW2 ||
                        state == S_CLIP || state == S_SPAN || state == S_ACC;
    wire        c_drain = state == S_IDLE && su_go && !su_accept;
    wire [31:0] c_next = (c_edge ? edge_clocks : c_row ? row_clocks : drain_clocks) + 1'b1;

always @(posedge clk) begin
    // a triangle from the CPU: 7..18 out of the RAM, a step's values a clock after its read
    if(ld == 3'd1) begin t_xr <= cr_qa[10:0]; t_yr <= cr_qa[26:16]; end
    if(ld == 3'd5) t_base <= cr_qa[25:0];
    if(ld == 3'd6) begin t_w <= cr_qa[8:0]; t_h <= cr_qa[24:16]; t_cut <= cr_qa[31]; end
    // dx, dy, a: shifted in at place 2, or (S_ACC) rotated so that k's values are at place 0
    // (three times, so they are in order again for S_TEX) and k - 1's a at place 2
    if(dx_in || acc_rot) begin t_dx[0] <= t_dx[1]; t_dx[1] <= t_dx[2]; t_dx[2] <= dx_in ? cr_qb : t_dx[0]; end
    if(dy_in || acc_rot) begin t_dy[0] <= t_dy[1]; t_dy[1] <= t_dy[2]; t_dy[2] <= dy_in ? cr_qa : t_dy[0]; end
    if(a_in || acc_rot) begin t_a[0] <= t_a[1]; t_a[1] <= t_a[2]; t_a[2] <= a_in ? cr_qb : t_a[0]; end

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
        if(c_edge) edge_clocks <= c_next;
        if(c_row) row_clocks <= c_next;
        if(c_drain) drain_clocks <= c_next;
    end
    if(wr && wr_addr == 5'd6) pixels <= 0;
    else if(fb_wr) pixels <= pixels + fb_be[0] + fb_be[1] + fb_be[2] + fb_be[3];

    case(state)
    S_IDLE: if(start || su_take) begin
        {ay, ax} <= pa;
        {by, bx} <= pb;
        {cy, cx} <= pc;
        if(su_go) begin
            colour <= su_draw[7:0];
            fb_buf <= su_draw[8];
            textured <= su_draw[9];
            t_base <= su_tex[25:0];
            t_w <= su_size[8:0];
            t_h <= su_size[24:16];
            t_cut <= su_size[31];
            t_xr <= su_ref[10:0];
            t_yr <= su_ref[26:16];
            t_a[0] <= su_a0; t_a[1] <= su_a1; t_a[2] <= su_a2;
            t_dx[0] <= su_dx0; t_dx[1] <= su_dx1; t_dx[2] <= su_dx2;
            t_dy[0] <= su_dy0; t_dy[1] <= su_dy1; t_dy[2] <= su_dy2;
        end else begin                          // (7..18 from the RAM, by ld)
            colour <= wr_data[7:0];
            fb_buf <= wr_data[8];
            textured <= wr_data[9];
        end
        cx0 <= clip_x0;
        cy0 <= clip_y0;
        cx1 <= clip_x1;
        cy1 <= clip_y1;
        state <= S_ROWS;
    end

    S_ROWS: begin
        r <= r0_c[7:0];                         // (0..199 when there are rows)
        go_none <= r0_c > cy || r0_c >= $signed({4'd0, cy1});
        go_alone <= r0_c >= cy;
        last0 <= by != cy ? cx : ay != cy ? bc_min : ax < bc_min ? ax : bc_min;
        last1 <= by != cy ? cx : ay != cy ? bc_max : ax > bc_max ? ax : bc_max;
        state <= S_GO;
    end

    S_GO: begin                                 // (a clock of its own: the rows are registers)
        if(go_none) state <= S_IDLE;            // nothing to draw
        else if(go_alone) state <= S_ROW;       // row c.y alone: no edge needed
        else begin
            edge_start;                         // a-c
            job <= 0;
            state <= S_EDGE;
        end
    end

    S_EDGE: begin
        case(e_phase)
        2'd0: begin                             // two quotient bits a clock
            if(e_cnt[3:1] != 3'b111) begin
                e_q <= {e_q[25:0], !e_sub1[13], !e_sub2[13]};
                e_rem <= e_rem2;
            end
            e_cnt <= e_cnt + 1'b1;
            if(e_cnt == 4'd15) e_phase <= 1;
        end
        2'd1: e_phase <= 2;
        2'd2: begin
            // |slope| >= 2^27 only for dy = 1, where r - y0 = 0
            e_prod <= e_q[26:0] * e_dy;
            e_phase <= 3;
        end
        default: begin
            if(job == 2'd0) begin l_x <= e_x; l_dx <= e_slope; end
            else if(job == 2'd1) begin s_x <= e_x; s_dx <= e_slope; end
            if(job == 2'd0) begin
                job <= 1;
                edge_start;                     // a-b if the first row is above b's, else b-c
            end else if(job == 2'd1 && r_s < by && by < cy && by < $signed({4'd0, cy1})) begin
                job <= 2;
                edge_start;                     // b-c from b.y
            end else
                state <= S_ROW;
        end
        endcase
    end

    S_ROW: begin
        long_left <= $signed({l_x, ~lb[27], lb[26:0]}) < $signed({s_x, ~sb[27], sb[26:0]});
        l_lo_t <= !l_d[12] || (l_d == -13'sd1 && !l_ft);
        l_hi_t <= l_d[12] || l_d == 13'sd0 || (l_d == 13'sd1 && !l_fb);
        s_lo_t <= !s_d[12] || (s_d == -13'sd1 && !s_ft);
        s_hi_t <= s_d[12] || s_d == 13'sd0 || (s_d == 13'sd1 && !s_fb);
        state <= S_ROW2;
    end

    S_ROW2: begin
        px0 <= e_end;
        state <= S_CLIP;
    end

    S_CLIP: begin
        px1 <= e_end;
        state <= S_SPAN;
    end

    S_SPAN: begin
        w1 <= px_last[8:2];
        x <= px0;
        first <= 1;
        acc_k <= 0;
        if(px0 >= px1) next_row;
        else state <= textured ? S_ACC : S_FILL;
    end

    S_FILL: begin                               // a word a clock: x moves on by 4
        flat_wr <= 1;
        flat_addr <= x_word;
        flat_be <= be;
        first <= 0;
        x <= x_next;
        if(x[8:2] == w1) next_row;
    end

    S_ACC: begin                                // products for k (at place 0), the sum one clock later
        acc_k <= acc_k + 1'b1;
        if(acc_k != 2'd3) begin
            acc_px <= t_dx[0] * acc_dx;
            acc_py <= t_dy[0] * acc_dy;
        end
        if(acc_k != 2'd0)
            acc[acc_k - 2'd1] <= $signed({{4{t_a[2][31]}}, t_a[2], 8'd0}) +
                                 $signed({acc_px[42], acc_px}) + $signed({acc_py[42], acc_py});
        if(acc_k == 2'd3) state <= S_TEX;
    end

    S_TEX: if(tex_ready) begin
        x <= x_next;
        acc[0] <= acc[0] + {{12{t_dx[0][31]}}, t_dx[0]};
        acc[1] <= acc[1] + {{12{t_dx[1][31]}}, t_dx[1]};
        acc[2] <= acc[2] + {{12{t_dx[2][31]}}, t_dx[2]};
        if(x == px_last) next_row;
    end

    default: state <= S_IDLE;
    endcase

    if(reset) state <= S_IDLE;
end

    // to the next row: both edges one row on, at the bottoms worked out for this row (those
    // differ from x + slope only where the next row is c.y, which takes its corners, or b.y);
    // from row b.y on the edge b-c, whose values are still in the edge unit (not needed when
    // b.y is c's row). The last row is c.y or the one above clip y1.
    task next_row;
        begin
            r <= r + 1'b1;
            l_x <= lb;
            if(r1_is_b) begin
                s_x <= e_x;
                s_dx <= e_slope;
            end else begin
                s_x <= sb;
            end
            state <= r_end ? S_IDLE : S_ROW;
        end
    endtask

endmodule
