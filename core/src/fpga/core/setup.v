//
// Setup unit (SU): turns polygons in view space into triangles with everything that the
// rasterizer (raster.v) needs, and starts them there. It is bit for bit the C model
// firmware/setup.c; setup.h has the arithmetic.
//
// Input is a polygon from the CPU (the registers below) or from the geometry unit (the gu_*
// ports, geom.v). Its values have 8 fraction bits as they are written; s and t are saturated
// at 24 bits. Output is one triangle at a time (the t_* ports; go and rs_accept are the
// handshake).
//
// Stages
//   clipper   Cuts the polygon at the near plane, the far plane and a guard band
//             (clipper.v).
//   corner    rc = 2^(16 + l) / f from the reciprocal table and one Newton step (l = bit
//             length of f). x = centre + scale r / f and y = centre - scale u / f, in 1/16
//             pixel. s rc and t rc are kept.
//   triangle  The polygon is split into a fan of triangles: corner 0 with each pair of
//             neighbours. For a textured triangle: d = twice the area (a triangle with
//             d = 0 is left out) and 1/|d| from the table; Q, U, V at the corners scaled by
//             the triangle's largest Q; per value the change per pixel in x and y (8
//             fraction bits) and the value at the reference pixel's centre.
//
// Schedule
//   After the clipper there are two parts, each with one multiplier (operands registered, two
//   stages, the result three clocks after it was issued) and a fixed schedule: the corner
//   part (about 20 clocks a corner) and the triangle part (about 40 clocks for a textured
//   triangle, 1 clock for a flat one). They work in parallel. While the rasterizer draws a
//   triangle, the triangle part works out the next one, the corner part the corner after it,
//   the clipper the next polygon, and the CPU writes the polygon after that.
//
// Storage
//   Wide values are kept in block RAM where a clock of read latency fits the schedule:
//   the corner in the corner part; rc, s rc and t rc of the triangle's corners (the corner
//   part writes them as they come, the triangle part reads one a clock through one
//   shifter); and the triangle's Q, U, V at corner 0, their differences and the changes per
//   pixel. Sums of two products are added in the multiplier's second stage.
//   The operands come from small selects on the step, without holding. Wide registers are
//   written through explicit enables: a register held in most states of a case would become
//   a wide mux of the state.
//
// Registers (soc.v maps them at 0xF000_3080 + 4 * wr_addr, docs/soc.md):
//   0   centre of the view: [15:0] x, [31:16] y in 1/16 pixel
//   1   [15:0] focal length in pixels times 16
//   2   mode: [7:0] colour, [8] buffer, [9] textured,    } taken with the first
//       [12:10] corners (3 or 4)                          } corner of a polygon
//   3   texture address, 4 texture size (raster.v's 7, 8) }
//   5..8  r, u, f, s of a corner (floats)
//   9   t, and the corner continues the polygon   10  t, and the corner starts a polygon:
//       hands the corner over (soc.v holds the write back while ready is low)
//   11  write: clear busy_clocks
//   12  the far plane's depth with 8 fraction bits (an integer)
//

module setup (
    input   wire            clk,
    input   wire            reset,

    input   wire            wr,
    input   wire    [3:0]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            ready,                  // the unit can take a corner write (registers 9, 10)
    output  wire            busy,                   // polygons, corners or triangles are in work
    output  reg     [31:0]  busy_clocks = 0,        // clocks computing since the last clear

    // A triangle for the rasterizer (raster.v). go is raised when the t_* values are valid and
    // stays high until the rasterizer takes the triangle in a clock with go and rs_accept.
    // The values are those of raster.v's registers:
    //   t_v0..t_v2  corners, [15:0] x, [31:16] y, in 1/16 pixel on whole pixels
    //   t_ref       reference pixel, [9:0] x, [25:16] y, in pixels
    //   t_tex       texture address, t_size texture size, t_draw colour, buffer and mode
    //   t_a0..t_a2  Q, U, V at the reference pixel's centre
    //   t_dx*, t_dy*  the change of Q, U, V per pixel in x and in y, 8 fraction bits
    output  reg             go = 0,
    input   wire            rs_accept,
    output  reg     [31:0]  t_v0, t_v1, t_v2,       // [15:0] x, [31:16] y
    output  reg     [31:0]  t_ref,
    output  reg     [31:0]  t_tex, t_size,
    output  reg     [9:0]   t_draw,
    output  reg     [31:0]  t_a0, t_a1, t_a2,
    output  reg     [31:0]  t_dx0, t_dx1, t_dx2,
    output  reg     [31:0]  t_dy0, t_dy1, t_dy2,

    // A polygon from the geometry unit (geom.v), taken instead of the CPU's corners in a
    // clock with gu_valid and gu_ready (the CPU writes none meanwhile). The formats are in
    // geom.v.
    input   wire            gu_valid,
    output  wire            gu_ready,
    input   wire    [2:0]   gu_n,
    input   wire    [9:0]   gu_mode,
    input   wire    [31:0]  gu_tex, gu_size,
    input   wire    [575:0] gu_c
);

//
// Reciprocal table, the same as in raster_tex.v: 1024 entries, r0[i] = floor((2^32 - 1) /
// (2^15 + 32 i + 16)). It has one read port for each part: ra_* for the corner part, rb_* for
// the triangle part.
//
    reg  [16:0] rtab [0:1023];
    integer     ti;
    reg  [32:0] tnum, tq;
    initial begin
        tnum = 33'h0FFFFFFFF;
        for(ti = 0; ti < 1024; ti = ti + 1) begin
            tq = tnum / (32768 + 32 * ti + 16);
            rtab[ti] = tq[16:0];
        end
    end
    reg  [9:0]  ra_addr, rb_addr;
    reg  [16:0] ra_q, rb_q;
    // both read ports, one clock after the address
always @(posedge clk) begin
    ra_q <= rtab[ra_addr];
    rb_q <= rtab[rb_addr];
end

    // Converts a float to a fixed-point value with 8 fraction bits (su_fix8): the mantissa m
    // shifted by e - 142. Here m << 7 is shifted right by 149 - e (0..30 for the exponents
    // 119..149, so nothing is lost).
    function signed [31:0] fix8(input [31:0] b);
        reg [31:0] a;
        reg [4:0]  sh;
        begin
            sh = 5'd21 - b[27:23];              // 149 - e
            if(b[30:23] >= 8'd150) a = 32'h7FFF_FFFF;
            else if(b[30:23] > 8'd118) a = {2'b01, b[22:0], 7'd0} >> sh;
            else a = 0;
            fix8 = b[31] ? -a : a;
        end
    endfunction

    // bitlen31: the bit length of a number below 2^31.
    // msb27: the position of the highest set bit of a number below 2^27.
    function [4:0] bitlen31(input [30:0] v);
        integer i;
        begin
            bitlen31 = 0;
            for(i = 0; i < 31; i = i + 1) if(v[i]) bitlen31 = i[4:0] + 5'd1;
        end
    endfunction
    function [4:0] msb27(input [26:0] v);
        integer i;
        begin
            msb27 = 0;
            for(i = 0; i < 27; i = i + 1) if(v[i]) msb27 = i[4:0];
        end
    endfunction

    // Saturation to fewer bits: the value itself where the bits above are its sign, else the
    // largest value of its sign.
    function signed [15:0] sat16(input signed [21:0] v);
        sat16 = v[21:15] == {7{v[21]}} ? v[15:0] : {v[21], {15{!v[21]}}};
    endfunction
    function signed [20:0] sat21(input signed [49:0] v);      // +-(2^20 - 1): -2^20 too
        sat21 = v[49:20] == {30{v[49]}} && !(v[49] && v[19:0] == 20'd0) ? v[20:0] :
                {v[49], {19{!v[49]}}, 1'b1};
    endfunction
    function signed [26:0] sat27(input signed [30:0] v);
        sat27 = v[30:26] == {5{v[30]}} ? v[26:0] : {v[30], {26{!v[30]}}};
    endfunction
    function signed [31:0] sat32(input signed [50:0] v);
        sat32 = v[50:31] == {20{v[50]}} ? v[31:0] : {v[50], {31{!v[50]}}};
    endfunction

//
// The registers the CPU writes. Floats are converted to 8 fraction bits as they are written.
//
    reg  signed [15:0] cen_x, cen_y;            // register 0
    reg  [15:0] scale;                          // register 1
    reg  [12:0] in_mode;                        // register 2
    reg  [31:0] in_tex, in_size, far8;          // registers 3, 4, 12
    reg  signed [31:0] in_r, in_u, in_f;        // registers 5, 6, 7
    reg  [23:0] in_s, in_t;                     // saturated to 24 bits as they are written
    wire signed [31:0] wr_fix = fix8(wr_data);
    // to 24 bits: from 2^23 on (e >= 142) saturated
    wire [23:0] wr_fix24 = wr_data[30:23] >= 8'd142 ? {wr_data[31], {23{!wr_data[31]}}} : wr_fix[23:0];
    // a write of register 9 or 10 hands a corner over
    wire        trigger = wr && (wr_addr == 4'd9 || wr_addr == 4'd10) && !reset;

    // register writes (the corner's t and the hand-over are in the next block)
always @(posedge clk) begin
    if(wr && wr_addr == 4'd0) begin cen_x <= wr_data[15:0]; cen_y <= wr_data[31:16]; end
    if(wr && wr_addr == 4'd1) scale <= wr_data[15:0];
    if(wr && wr_addr == 4'd2) in_mode <= wr_data[12:0];
    if(wr && wr_addr == 4'd3) in_tex <= wr_data;
    if(wr && wr_addr == 4'd4) in_size <= wr_data;
    if(wr && wr_addr == 4'd5) in_r <= wr_fix;
    if(wr && wr_addr == 4'd6) in_u <= wr_fix;
    if(wr && wr_addr == 4'd7) in_f <= wr_fix;
    if(wr && wr_addr == 4'd8) in_s <= wr_fix24;
    if(wr && wr_addr == 4'd12) far8 <= wr_data;
end

//
// The polygon being written (ib). A corner goes in one clock after its last write
// (register 9 or 10). When the polygon is complete, the clipper copies it out. ib is in
// registers: block RAM would save logic but is the scarcer resource.
//
    reg  [575:0] ib;
    reg  [2:0]  ib_n, ib_cnt;                   // corners of the polygon; corners so far
    reg  [9:0]  ib_mode;
    reg  [31:0] ib_tex, ib_size;
    reg         ib_full = 0, tw = 0, tw_first;  // ib_full: a polygon waits for the clipper;
                                                // tw: a corner goes in now, tw_first: it is the first
    reg  [9:0]  cl_mode;                        // the polygon in the clipper
    reg  [31:0] cl_tex, cl_size;
    wire        cl_idle, cl_taken, co_valid, co_first;
    wire [143:0] co_c;
    wire        co_take;                        // the corner part takes a corner
    wire [2:0]  tw_idx = tw_first ? 3'd0 : ib_cnt;
    // the number of corners, limited to 3..4, taken from the mode with the first corner
    wire [2:0]  tw_n = tw_first ? (in_mode[12:10] < 3'd3 ? 3'd3 :
                                   in_mode[12:10] > 3'd4 ? 3'd4 : in_mode[12:10]) : ib_n;

    // corners into ib, from the CPU or the geometry unit, and the polygon's data into the
    // clipper's stage (cl_*)
always @(posedge clk) begin
    if(trigger && ready) begin
        in_t <= wr_fix24;
        tw <= 1;
        tw_first <= wr_addr == 4'd10;
    end
    if(tw) begin
        tw <= 0;
        ib[144 * tw_idx +: 144] <= {in_t, in_s, in_f, in_u, in_r};
        ib_cnt <= tw_idx + 3'd1;
        if(tw_first) begin
            ib_n <= tw_n;
            ib_mode <= in_mode[9:0];
            ib_tex <= in_tex;
            ib_size <= in_size;
        end
        if(tw_idx == tw_n - 3'd1) ib_full <= 1;
    end
    if(gu_valid && gu_ready) begin
        ib <= gu_c;
        ib_n <= gu_n;
        ib_mode <= gu_mode;
        ib_tex <= gu_tex;
        ib_size <= gu_size;
        ib_full <= 1;
    end
    if(ib_full && cl_idle) begin
        cl_mode <= ib_mode;
        cl_tex <= ib_tex;
        cl_size <= ib_size;
    end
    if(cl_taken) ib_full <= 0;
    if(reset) begin
        tw <= 0;
        ib_full <= 0;
    end
end

clipper cl (
    .clk                ( clk ),
    .reset              ( reset ),
    .start              ( ib_full ),
    .n                  ( ib_n ),
    .in_c               ( ib ),
    .far8               ( far8 ),
    .idle               ( cl_idle ),
    .taken              ( cl_taken ),
    .out_valid          ( co_valid ),
    .out_first          ( co_first ),
    .out_c              ( co_c ),
    .out_take           ( co_take )
);

//
// Corner part: one multiplier and one shifter, right by l. The shifter brings f to 16 bits
// for the table (steps 2 and 4), and r rc, u rc to r / f, u / f where those products come
// back. Of the corner, f is kept in a register, and r, u, s, t in a block RAM, read from
// step 1 on. rc, s rc and t rc go into the corner store (slot cs_w) as they come.
// A corner takes about 20 clocks: cu_step runs from 0 to 20.
//
    reg         cu_busy = 0, cu_done = 0;
    reg  [4:0]  cu_step;
    (* ramstyle = "no_rw_check, M10K" *) reg  [111:0] cin [0:1];    // two words: one is read
    reg  [111:0] cin_q;
    reg         cin_w = 0;                      // the word written last
    // Input of the corner part: the clipper's corner (r, u, s, t) in two words that are
    // written alternately. The corner part reads the word written last.
always @(posedge clk) begin
    if(co_take) begin
        cin[!cin_w] <= {co_c[143:96], co_c[63:0]};
        cin_w <= !cin_w;
    end
    cin_q <= cin[cin_w];
end
    wire signed [31:0] c_r = cin_q[31:0], c_u = cin_q[63:32];
    wire signed [23:0] c_s = cin_q[87:64], c_t = cin_q[111:88];
    reg  [30:0] c_f;                            // at least 1, so l <= 31
    reg         c_first;                        // the corner starts a polygon
    reg  [4:0]  c_l;                            // the bit length of c_f
    reg  [16:0] c_rc;                           // rc
    reg  signed [15:0] c_x, c_y;                // the projected corner, 1/16 pixel on whole pixels

    // The corner part's multiplier and the work that its result is for (cm_t). The steps
    // cu_step that issue a product are:
    //   4       m x r0 (no tag, the product is read at step 7)
    //   7       e x r0 (C_R2)
    //   11..14  s, r, u, t x rc (C_SR, C_P, C_P, C_TR)
    //   15, 16  the projections x and y: r / f and u / f times scale (C_X, C_Y). The scale
    //           is read at these steps, the centre when the result comes back.
    //   17      rc x 2^16 (C_Q)
    // The corner store takes bits 40..10 of the results C_SR, C_TR and C_Q.
    localparam  [2:0]   C_R2 = 3'd1, C_P = 3'd2, C_SR = 3'd3, C_TR = 3'd4, C_Q = 3'd5,
                        C_X = 3'd6, C_Y = 3'd7;
    reg  signed [31:0] cm_a;
    reg  signed [17:0] cm_b;
    reg  signed [49:0] cm_p1, cm_p2;
    reg  [2:0]  cm_t0 = 0, cm_t1 = 0, cm_t2 = 0;
    reg  [2:0]  cm_t;

    wire        c_proj = cm_t2 == C_P;
    wire [49:0] csh_in = c_proj ? cm_p2 : {3'd0, c_f, 16'd0};
    wire signed [49:0] csh = $signed(csh_in) >>> c_l;
    wire [15:0] c_m = csh[15:0];                // norm16(f, l)
    wire signed [20:0] c_p = sat21(csh);        // r / f or u / f with 16 fraction bits
    // e = 2^32 - m r0, |e| < 2^22, so r0 (2^33 - m r0) = r0 2^32 + r0 e: rc = r0 + (r0 e >> 32)
    wire [22:0] c_e = -cm_p2[22:0];
    wire [17:0] c_rc_n = {1'b0, ra_q} + cm_p2[49:32];
    // on whole pixels, the fraction cut off as the game's projection does (0x83420); the
    // products are below 2^36
    wire [21:0] cm_xw = cm_p2[37:16] + {{6{cen_x[15]}}, cen_x};
    wire [37:0] cm_yw = {{6{cen_y[15]}}, cen_y, 16'd0} - cm_p2[37:0];
    wire [15:0] cm_xs = sat16(cm_xw), cm_ys = sat16(cm_yw[37:16]);

    wire        cu_take;                        // the triangle part takes the corner

    // the tag of the product that the current step issues
always @* begin
    case(cu_step)
    5'd7:  cm_t = C_R2;
    5'd11: cm_t = C_SR;
    5'd12: cm_t = C_P;
    5'd13: cm_t = C_P;
    5'd14: cm_t = C_TR;
    5'd15: cm_t = C_X;
    5'd16: cm_t = C_Y;
    5'd17: cm_t = C_Q;
    default: cm_t = 3'd0;
    endcase
end

    // The corner part's multiplier operands and tags (cm_t0 .. cm_t2 follow the product through
    // the two stages), the start of a corner, the step counter and the results that are
    // kept: c_rc, c_x, c_y.
always @(posedge clk) begin
    case(cu_step)
    5'd4:  cm_a <= {16'd0, c_m};
    5'd7:  cm_a <= {{9{c_e[22]}}, c_e};
    5'd11: cm_a <= {{8{c_s[23]}}, c_s};
    5'd12: cm_a <= c_r;
    5'd13: cm_a <= c_u;
    5'd14: cm_a <= {{8{c_t[23]}}, c_t};
    5'd17: cm_a <= 32'sd65536;
    default: cm_a <= {{11{c_p[20]}}, c_p};
    endcase
    cm_b <= !cu_step[4] && !cu_step[3] ? {1'b0, ra_q} :
            cu_step == 5'd15 || cu_step == 5'd16 ? {2'd0, scale} : {1'b0, c_rc};
    cm_t0 <= cu_busy ? cm_t : 3'd0;
    cm_p1 <= cm_a * cm_b;
    cm_p2 <= cm_p1;
    cm_t1 <= cm_t0;
    cm_t2 <= cm_t1;

    if(co_take) begin
        c_f <= co_c[95] || co_c[94:64] == 31'd0 ? 31'd1 : co_c[94:64];
        c_first <= co_first;
        cu_busy <= 1;
        cu_step <= 0;
    end

    if(cu_busy) begin
        cu_step <= cu_step + 1'b1;
        if(cu_step == 5'd1) c_l <= bitlen31(c_f);
        if(cu_step == 5'd2) ra_addr <= c_m[14:5];
        if(cu_step == 5'd20) begin
            cu_busy <= 0;
            cu_done <= 1;
        end
    end

    if(cm_t2 == C_R2) c_rc <= c_rc_n[16:0];
    if(cm_t2 == C_X) c_x <= {cm_xs[15:4], 4'd0};
    if(cm_t2 == C_Y) c_y <= {cm_ys[15:4], 4'd0};

    if(cu_take) cu_done <= 0;
    if(reset) begin
        cu_busy <= 0;
        cu_done <= 0;
    end
end

//
// Triangle part: corner 0 (the polygon's first), corner 1 (the one before) and corner 2
// (the new one). The corner part hands over a corner (cu_take) when the triangle part is
// idle and the rasterizer has taken the previous triangle (go is low). The third corner
// starts a triangle. The step tu_step runs while tu_busy:
//   0         The corners and the draw word go to the t_* outputs. A flat triangle is
//             done here (go).
//   1         The reference pixel.
//   1..10     Q, U, V of the corners are read from the corner store, shifted to a common
//             scale and stored as differences from corner 0 in the operand store.
//   6         A textured triangle without area (d = 0) is left out.
//   1..36     The multiplier works out d, 1/|d| and, for each of Q, U, V, the changes per
//             pixel and the value at the reference pixel (see the list below).
//   40        A textured triangle is done (go).
//
    reg  signed [15:0] px0, py0, px1, py1, px2, py2;
    reg  [4:0]  pl0, pl1, pl2;
    // The corner store's slots of corners 0, 1, 2 and of the corner in the corner part. A
    // corner taken swaps slots with the corner it replaces. Corner 2 becoming corner 1 swaps
    // the two.
    reg  [1:0]  cs_0 = 2'd0, cs_1 = 2'd1, cs_2 = 2'd2, cs_w = 2'd3;
    reg  [9:0]  p_mode;                         // the polygon's, from the clipper: it holds
    reg  [31:0] p_tex, p_size;                  // the polygon while its first corner is taken
    reg  [1:0]  p_have = 0;                     // corners of the polygon so far, up to 2
    reg         tu_busy = 0;
    reg  [5:0]  tu_step;

    assign cu_take = cu_done && !tu_busy && !go;
    assign co_take = co_valid && !cu_busy && !cu_done;
    assign ready = !ib_full && !tw;
    assign gu_ready = !ib_full && !tw;
    assign busy = tw || ib_full || !cl_idle || cu_busy || cu_done || tu_busy || go;

    // The corner store (cst): at {kind, slot} rc << 6, s rc >> 10, t rc >> 10 (kind 0, 1, 2; the
    // shifter drops 10 bits of s rc and t rc anyway). It is read one clock after the address.
    (* ramstyle = "no_rw_check, M10K" *) reg  [30:0] cst [0:15];
    wire        cst_we = cm_t2 == C_Q || cm_t2 == C_SR || cm_t2 == C_TR;
    wire [1:0]  cst_wk = cm_t2 == C_Q ? 2'd0 : cm_t2 == C_SR ? 2'd1 : 2'd2;
    reg  [30:0] cst_q;

    // Q, U, V at corners 0, 1, 2 through one shifter, one value a clock. A value is read at
    // steps 1..8, shifted into sv a clock later, and written into the operand store one more
    // clock later. All values are shifted right to the scale of the smallest l (the nearest
    // corner), which is the scaling by the triangle's largest Q.
    reg  [1:0]  rd_k, rd_c;                     // kind and corner of the value read
    // which value (Q, U or V of which corner) the corner store reads in each step
    always @(*) begin
        case(tu_step)
        6'd1: begin rd_k = 2'd0; rd_c = 2'd1; end
        6'd2: begin rd_k = 2'd0; rd_c = 2'd2; end
        6'd3: begin rd_k = 2'd1; rd_c = 2'd0; end
        6'd4: begin rd_k = 2'd1; rd_c = 2'd1; end
        6'd5: begin rd_k = 2'd1; rd_c = 2'd2; end
        6'd6: begin rd_k = 2'd2; rd_c = 2'd0; end
        6'd7: begin rd_k = 2'd2; rd_c = 2'd1; end
        6'd8: begin rd_k = 2'd2; rd_c = 2'd2; end
        default: begin rd_k = 2'd0; rd_c = 2'd0; end
        endcase
    end
    wire [1:0]  rd_slot = rd_c == 2'd0 ? cs_0 : rd_c == 2'd1 ? cs_1 : cs_2;
    wire [4:0]  rd_l = rd_c == 2'd0 ? pl0 : rd_c == 2'd1 ? pl1 : pl2;
    wire [4:0]  lm01 = pl0 < pl1 ? pl0 : pl1;
    wire [4:0]  lmin = pl2 < lm01 ? pl2 : lm01;
    // corner store: write from the corner part's results, read for the triangle part
always @(posedge clk) begin
    if(cst_we) cst[{cst_wk, cs_w}] <= cm_p2[40:10];
    cst_q <= cst[{rd_k, rd_slot}];
end
    reg  [4:0]  sh_d;                           // l - the smallest l of the value read
    wire signed [30:0] sh_out = $signed(cst_q) >>> sh_d;
    reg  signed [26:0] sv, sv0;                 // the value; Q, U or V at corner 0
    wire        sv_0 = tu_step == 6'd2 || tu_step == 6'd5 || tu_step == 6'd8;
    wire [27:0] sv_d = {sv[26], sv} - (sv_0 ? 28'd0 : {sv0[26], sv0});

    // Values per triangle. dx1 = x1 - x0, dx2 = x2 - x0, dy2 = y2 - y0 and dy1 = y1 - y0 are
    // the edges from corner 0 in 1/16 pixel.
    reg  signed [16:0] dx1, dx2, dy2, ndx2, ndy1;   // ndx2 = -dx2, ndy1 = -dy1
    reg  signed [17:0] fx, fy;                  // reference pixel's centre less corner 0, 1/16 pixel
    reg  [26:0] tad;                            // |d| / 256 (d, of multiples of 16, is one of 256)
    reg         d_neg;
    reg  [4:0]  lds;                            // ld - 8, ld the bit length of |d| (9..35)
    reg  signed [17:0] trcd;                    // 2^(16 + ld) / |d| with the sign of d
    // The plane's normal of the value being done, bits 44..4. dx and dy are multiples of 16
    // because the corners are on whole pixels, so are nx and ny.
    reg  signed [40:0] t_nx, t_ny;

    // The triangle part's multiplier, and the work that a result is for (tm_t). A step
    // without a tag issues a product that is only added to the next one, or read directly.
    localparam  [3:0]   T_D2 = 4'd1, T_R2 = 4'd2, T_NX2 = 4'd3, T_NY2 = 4'd4, T_GX = 4'd5,
                        T_GY = 4'd6, T_A1 = 4'd7, T_A2 = 4'd8;
    reg  signed [44:0] tm_a;
    reg  signed [17:0] tm_b;
    reg  signed [62:0] tm_p1, tm_p2;
    reg  [3:0]  tm_t0 = 0, tm_t1 = 0, tm_t2 = 0;
    reg  [1:0]  tm_k0, tm_k1, tm_k2;

    // The operand store, block RAM, read a clock after its address. It holds, for Q, U, V
    // (k = 0, 1, 2):
    //   OA_A0 + k  the value at corner 0                        (from the shifter, steps 2..10)
    //   OA_D1 + k  the value at corner 1 minus that at corner 0 (from the shifter)
    //   OA_D2 + k  the value at corner 2 minus that at corner 0 (from the shifter)
    //   OA_GX + k  the change per pixel in x                    (from the results)
    //   OA_GY + k  the change per pixel in y                    (from the results)
    localparam  [4:0]   OA_A0 = 5'd0, OA_D1 = 5'd4, OA_D2 = 5'd8, OA_GX = 5'd12, OA_GY = 5'd16;
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] opa [0:31];
    reg  [31:0] opa_q;
    reg  [4:0]  opa_ra, opa_ws;

    // One shifter for the results, right by ld + 4 (the changes per pixel; the rounding is
    // in tm_p2) or by 12 (the values at the reference pixel). At steps 7 and 9 it also brings
    // |d| to 16 bits (norm16) for the table. ld >= 9 (d is a multiple of 256), so 12 bits
    // are always dropped and lds is the rest.
    wire        tg_res = tm_t2 == T_GX || tm_t2 == T_GY || tm_t2 == T_A2;
    wire [50:0] tg_in = tg_res ? tm_p2[62:12] : {8'd0, tad, 16'd0};
    wire signed [50:0] tg_sh = $signed(tg_in) >>> (tm_t2 == T_A2 ? 5'd0 : lds);
    wire signed [31:0] tg_sat = sat32(tg_sh);

    // Added to the product in the second stage: the rounding 2^(ld + 3) of the changes per
    // pixel; 4096 a0 + 2048 to the first product of a value at the reference pixel; for the
    // second product of a sum, the product before.
    wire [62:0] t_rnd = 63'd2048 << lds;
    wire [62:0] tm_add = tm_t1 == T_GX || tm_t1 == T_GY ? t_rnd :
                         tm_t1 == T_A1 ? {{19{opa_q[31]}}, opa_q, 12'h800} :
                         tm_t1 == T_D2 || tm_t1 == T_NX2 || tm_t1 == T_NY2 || tm_t1 == T_A2 ? tm_p2 : 63'd0;

    wire        opa_wg = tm_t2 == T_GX || tm_t2 == T_GY;
    wire        opa_we = opa_wg || (tu_busy && tu_step >= 6'd2 && tu_step <= 6'd10);
    wire [4:0]  opa_wa = opa_wg ? (tm_t2 == T_GY ? OA_GY : OA_GX) + {3'd0, tm_k2} : opa_ws;
    wire [31:0] opa_wd = opa_wg ? tg_sat : {{4{sv_d[27]}}, sv_d};
    // operand store: write port (shifter value or result) and read port (opa_q)
always @(posedge clk) begin
    if(opa_we) opa[opa_wa] <= opa_wd;
    opa_q <= opa[opa_ra];
end

    // The work of a step (op_t, op_k, op_a, op_b choose the multiplier's operands and tag):
    //   steps 1, 2   d = dx1 dy2 + dx2 (-dy1)
    //   steps 9, 12  R1, R2: the reciprocal of |d|
    //   per value k (Q, U, V)  nx = da1 dy2 + da2 (-dy1), ny = da2 dx1 + da1 (-dx2), then
    //                GX = nx rcd and GY = ny rcd (rcd is 1/|d| with the sign of d), and the
    //                value at the reference pixel a0 + (gx fx + gy fy + 2048) >> 12 (A1, A2)
    // da1 and da2 are the differences of the value at corners 1 and 2 from corner 0.
    localparam  [2:0]   A_DX1 = 3'd0, A_DX2 = 3'd1, A_MD = 3'd2, A_R2 = 3'd3, A_OPA = 3'd4,
                        A_NX = 3'd5, A_NY = 3'd6, A_GY2 = 3'd7;
    localparam  [2:0]   B_DY2 = 3'd0, B_NDY1 = 3'd1, B_DX1 = 3'd2, B_NDX2 = 3'd3, B_R0 = 3'd4,
                        B_RCD = 3'd5, B_FX = 3'd6, B_FY = 3'd7;
    reg  [3:0]  op_t;
    reg  [1:0]  op_k;
    reg  [2:0]  op_a, op_b;
    // the tag and operands of the multiplier in each step
    always @(*) begin
        op_t = 4'd0; op_k = 2'd0; op_a = A_OPA; op_b = B_DY2;
        case(tu_step)
        6'd1:  begin op_a = A_DX1; op_b = B_DY2; end
        6'd2:  begin op_t = T_D2; op_a = A_DX2; op_b = B_NDY1; end
        6'd9:  begin op_a = A_MD; op_b = B_R0; end
        6'd10: op_b = B_DY2;                                        // nx, Q
        6'd11: begin op_t = T_NX2; op_b = B_NDY1; end
        6'd12: begin op_t = T_R2; op_a = A_R2; op_b = B_R0; end
        6'd13: op_b = B_DX1;                                        // ny, Q
        6'd14: begin op_t = T_NY2; op_b = B_NDX2; end
        6'd15: op_b = B_DY2;                                        // nx, U
        6'd16: begin op_t = T_NX2; op_b = B_NDY1; end
        6'd17: begin op_t = T_GX; op_k = 2'd0; op_a = A_NX; op_b = B_RCD; end
        6'd18: begin op_t = T_GY; op_k = 2'd0; op_a = A_NY; op_b = B_RCD; end
        6'd19: op_b = B_DX1;                                        // ny, U
        6'd20: begin op_t = T_NY2; op_b = B_NDX2; end
        6'd21: op_b = B_DY2;                                        // nx, V
        6'd22: begin op_t = T_NX2; op_b = B_NDY1; end
        6'd23: begin op_t = T_GX; op_k = 2'd1; op_a = A_NX; op_b = B_RCD; end
        6'd24: begin op_t = T_A1; op_b = B_FX; end
        6'd25: begin op_t = T_A2; op_k = 2'd0; op_b = B_FY; end
        6'd26: begin op_t = T_GY; op_k = 2'd1; op_a = A_NY; op_b = B_RCD; end
        6'd27: op_b = B_DX1;                                        // ny, V
        6'd28: begin op_t = T_NY2; op_b = B_NDX2; end
        6'd29: begin op_t = T_GX; op_k = 2'd2; op_a = A_NX; op_b = B_RCD; end
        6'd30: begin op_t = T_A1; op_b = B_FX; end
        6'd31: begin op_t = T_A2; op_k = 2'd1; op_b = B_FY; end
        6'd32: begin op_t = T_GY; op_k = 2'd2; op_a = A_NY; op_b = B_RCD; end
        6'd35: begin op_t = T_A1; op_b = B_FX; end
        6'd36: begin op_t = T_A2; op_k = 2'd2; op_a = A_GY2; op_b = B_FY; end   // gy is not in
        default: ;                                                              // the store yet
        endcase
    end

    // what the next step reads from the operand store (opa_ra)
    always @(*) begin
        case(tu_step)
        6'd9:  opa_ra = OA_D1 + 5'd0;
        6'd10: opa_ra = OA_D2 + 5'd0;
        6'd12: opa_ra = OA_D2 + 5'd0;
        6'd13: opa_ra = OA_D1 + 5'd0;
        6'd14: opa_ra = OA_D1 + 5'd1;
        6'd15: opa_ra = OA_D2 + 5'd1;
        6'd18: opa_ra = OA_D2 + 5'd1;
        6'd19: opa_ra = OA_D1 + 5'd1;
        6'd20: opa_ra = OA_D1 + 5'd2;
        6'd21: opa_ra = OA_D2 + 5'd2;
        6'd23: opa_ra = OA_GX + 5'd0;
        6'd24: opa_ra = OA_GY + 5'd0;
        6'd25: opa_ra = OA_A0 + 5'd0;
        6'd26: opa_ra = OA_D2 + 5'd2;
        6'd27: opa_ra = OA_D1 + 5'd2;
        6'd29: opa_ra = OA_GX + 5'd1;
        6'd30: opa_ra = OA_GY + 5'd1;
        6'd31: opa_ra = OA_A0 + 5'd1;
        6'd34: opa_ra = OA_GX + 5'd2;
        6'd36: opa_ra = OA_A0 + 5'd2;
        default: opa_ra = 5'd0;
        endcase
    end

    // where the shifter's value goes at steps 2..10 (opa_ws)
    always @(*) begin
        case(tu_step)
        6'd3:  opa_ws = OA_D1 + 5'd0;
        6'd4:  opa_ws = OA_D2 + 5'd0;
        6'd5:  opa_ws = OA_A0 + 5'd1;
        6'd6:  opa_ws = OA_D1 + 5'd1;
        6'd7:  opa_ws = OA_D2 + 5'd1;
        6'd8:  opa_ws = OA_A0 + 5'd2;
        6'd9:  opa_ws = OA_D1 + 5'd2;
        6'd10: opa_ws = OA_D2 + 5'd2;
        default: opa_ws = OA_A0 + 5'd0;
        endcase
    end

    // The reference pixel: corner 0's, limited to the screen (320 x 200).
    wire [9:0]  xr = px0[15] ? 10'd0 : px0[15:4] > 12'd319 ? 10'd319 : {1'b0, px0[12:4]};
    wire [9:0]  yr = py0[15] ? 10'd0 : py0[15:4] > 12'd199 ? 10'd199 : {1'b0, py0[12:4]};

`ifdef GEOM_STATS
    // counters for the simulation: where the clocks go (printed below)
    integer     st_cl = 0, st_cu = 0, st_tu = 0, st_rs = 0, st_cw = 0, st_tri = 0;
`endif

    // The triangle part's multiplier (operands from the lists above), the tags through the
    // two stages, the busy counter, and the triangle part's control: taking a corner,
    // running the steps, collecting the results into the t_* outputs.
always @(posedge clk) begin
    case(op_a)
    A_DX1: tm_a <= {{28{dx1[16]}}, dx1};
    A_DX2: tm_a <= {{28{dx2[16]}}, dx2};
    A_MD:  tm_a <= {29'd0, tg_sh[15:0]};
    A_R2:  tm_a <= {11'd0, 34'h2_0000_0000 - tm_p2[33:0]};
    A_NX:  tm_a <= {t_nx, 4'd0};
    A_NY:  tm_a <= {t_ny, 4'd0};
    A_GY2: tm_a <= {{13{t_dy2[31]}}, t_dy2};
    default: tm_a <= {{13{opa_q[31]}}, opa_q};
    endcase
    case(op_b)
    B_DY2:  tm_b <= {dy2[16], dy2};
    B_NDY1: tm_b <= {ndy1[16], ndy1};
    B_DX1:  tm_b <= {dx1[16], dx1};
    B_NDX2: tm_b <= {ndx2[16], ndx2};
    B_R0:   tm_b <= {1'b0, rb_q};
    B_RCD:  tm_b <= trcd;
    B_FX:   tm_b <= fx;
    default: tm_b <= fy;
    endcase
    tm_t0 <= tu_busy ? op_t : 4'd0;
    tm_k0 <= op_k;
    tm_p1 <= tm_a * tm_b;
    tm_p2 <= tm_p1 + tm_add;
    tm_t1 <= tm_t0;
    tm_t2 <= tm_t1;
    tm_k1 <= tm_k0;
    tm_k2 <= tm_k1;

    if(!cl_idle || cu_busy || tu_busy) busy_clocks <= busy_clocks + 1'b1;
    if(wr && wr_addr == 4'd11) busy_clocks <= 0;
`ifdef GEOM_STATS
    // For the simulation only: where the clocks go, printed when the CPU clears busy_clocks.
    if(wr && wr_addr == 4'd11) begin
        $display("su: busy %0d, clipper %0d, corners %0d, triangles %0d, a triangle waiting for the rasterizer %0d, a corner waiting for the triangle part %0d, triangles out %0d",
                 busy_clocks, st_cl, st_cu, st_tu, st_rs, st_cw, st_tri);
        st_cl <= 0; st_cu <= 0; st_tu <= 0; st_rs <= 0; st_cw <= 0; st_tri <= 0;
    end else begin
        if(!cl_idle) st_cl <= st_cl + 1;
        if(cu_busy) st_cu <= st_cu + 1;
        if(tu_busy) st_tu <= st_tu + 1;
        if(go && !rs_accept) st_rs <= st_rs + 1;
        if(cu_done && !cu_take) st_cw <= st_cw + 1;
        if(go && rs_accept) st_tri <= st_tri + 1;
    end
`endif

    // A corner from the corner part. The first corner of a polygon becomes corner 0, the
    // next one corner 1, and the third one corner 2, which starts a triangle.
    if(cu_take) begin
        if(c_first || p_have == 2'd0) begin
            px0 <= c_x; py0 <= c_y; pl0 <= c_l;
            cs_0 <= cs_w; cs_w <= cs_0;
            p_mode <= cl_mode; p_tex <= cl_tex; p_size <= cl_size;
            p_have <= c_first ? 2'd1 : 2'd0;    // a polygon begins with a first corner
        end else if(p_have == 2'd1) begin
            px1 <= c_x; py1 <= c_y; pl1 <= c_l;
            cs_1 <= cs_w; cs_w <= cs_1;
            p_have <= 2'd2;
        end else begin
            px2 <= c_x; py2 <= c_y; pl2 <= c_l;
            cs_2 <= cs_w; cs_w <= cs_2;
            tu_busy <= 1;
            tu_step <= 0;
        end
    end

    // The triangle: the corners and the draw word for every triangle, the planes for a
    // textured one.
    sv <= sat27(sh_out);
    if(tu_busy) begin
        tu_step <= tu_step + 1'b1;
        sh_d <= rd_l - lmin;
        if(sv_0) sv0 <= sv;
        case(tu_step)
        6'd0: begin
            t_v0 <= {py0, px0};
            t_v1 <= {py1, px1};
            t_v2 <= {py2, px2};
            t_tex <= p_tex;
            t_size <= p_size;
            t_draw <= p_mode;
            if(!p_mode[9]) begin
                tu_busy <= 0;
                go <= 1;
            end
            dx1 <= px1 - px0; dx2 <= px2 - px0; dy2 <= py2 - py0;
            ndx2 <= px0 - px2; ndy1 <= py0 - py1;
        end
        6'd1: begin                             // the reference pixel and its centre
            fx <= $signed({5'd0, xr[8:0], 4'd8}) - {{2{px0[15]}}, px0};
            fy <= $signed({5'd0, yr[8:0], 4'd8}) - {{2{py0[15]}}, py0};
            t_ref <= {{6{yr[9]}}, yr, {6{xr[9]}}, xr};
        end
        6'd6: begin                             // no area: left out
            if(tad == 0) tu_busy <= 0;
            lds <= msb27(tad) + 5'd1;
        end
        6'd7: rb_addr <= tg_sh[14:5];
        6'd40: begin tu_busy <= 0; go <= 1; end
        default: ;
        endcase
    end

    case(tm_t2)
    T_D2: begin
        tad <= tm_p2[34] ? -tm_p2[34:8] : tm_p2[34:8];
        d_neg <= tm_p2[34];
    end
    T_R2:  trcd <= d_neg ? -{1'b0, tm_p2[48:32]} : {1'b0, tm_p2[48:32]};
    T_NX2: t_nx <= tm_p2[44:4];
    T_NY2: t_ny <= tm_p2[44:4];
    T_GX: begin
        case(tm_k2)
        2'd0: t_dx0 <= tg_sat;
        2'd1: t_dx1 <= tg_sat;
        default: t_dx2 <= tg_sat;
        endcase
    end
    T_GY: begin
        case(tm_k2)
        2'd0: t_dy0 <= tg_sat;
        2'd1: t_dy1 <= tg_sat;
        default: t_dy2 <= tg_sat;
        endcase
    end
    T_A2: begin
        case(tm_k2)
        2'd0: t_a0 <= tg_sat;
        2'd1: t_a1 <= tg_sat;
        default: t_a2 <= tg_sat;
        endcase
    end
    default: ;
    endcase

    // The rasterizer takes the triangle, or a textured triangle without area is left out.
    // Either way the new corner is the next triangle's corner 1.
    if(go && rs_accept) go <= 0;
    if((go && rs_accept) || (tu_busy && tu_step == 6'd6 && tad == 0)) begin
        px1 <= px2; py1 <= py2; pl1 <= pl2;
        cs_1 <= cs_2; cs_2 <= cs_1;
    end

    if(reset) begin
        tu_busy <= 0;
        go <= 0;
        p_have <= 0;
    end
end

endmodule
