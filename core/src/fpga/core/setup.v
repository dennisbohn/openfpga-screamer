//
// Setup unit: polygons in view space from the CPU, cut at the near plane, the far plane
// and a guard band (clipper.v), projected, split into a fan of triangles (corner 0 with each
// pair of neighbours), each with what the rasterizer needs, started there. Bit for bit the
// C model firmware/setup.c (setup.h has the arithmetic):
//   polygon: values to 8 fraction bits as they are written (s, t saturated at 24 bits);
//     cut (clipper.v)
//   corner: rc = 2^(16 + l) / f from the
//     reciprocal table and one Newton step (l = bit length of f); x = centre + scale r / f,
//     y = centre - scale u / f in 1/16 pixel; s rc, t rc kept
//   triangle: d = twice the area (none if 0, textured), 1/|d| from the table; Q, U, V at the
//     corners scaled by the triangle's largest Q; per value the change per pixel in x and y
//     (8 fraction bits) and the value at the reference pixel's centre
// After the clipper two parts, each with one multiplier (operands registered, two stages,
// the result three clocks after it was issued) and a fixed schedule: the corner part (about
// 20 clocks a corner) and the triangle part (about 40 clocks a textured triangle, 1 a flat
// one). While the rasterizer draws a triangle, the triangle part works out the next one,
// the corner part the corner after it, the clipper the next polygon, and the CPU writes the
// polygon after that.
//
// Registers (soc.v maps them at 0xF000_3080 + 4 * wr_addr):
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
    output  wire            ready,                  // a corner (9, 10) is taken
    output  wire            busy,                   // corners or triangles in work
    output  reg     [31:0]  busy_clocks = 0,        // clocks computing since the last clear

    // a triangle for the rasterizer: taken in a clock with go and rs_accept
    output  reg             go = 0,
    input   wire            rs_accept,
    output  reg     [31:0]  t_v0, t_v1, t_v2,       // [15:0] x, [31:16] y
    output  reg     [31:0]  t_ref,
    output  reg     [31:0]  t_tex, t_size,
    output  reg     [9:0]   t_draw,
    output  reg     [31:0]  t_a0, t_a1, t_a2,
    output  reg     [31:0]  t_dx0, t_dx1, t_dx2,
    output  reg     [31:0]  t_dy0, t_dy1, t_dy2,

    // a polygon from the geometry unit (geom.v), taken instead of the CPU's corners in a
    // clock with gu_valid and gu_ready (the CPU writes none meanwhile)
    input   wire            gu_valid,
    output  wire            gu_ready,
    input   wire    [2:0]   gu_n,
    input   wire    [9:0]   gu_mode,
    input   wire    [31:0]  gu_tex, gu_size,
    input   wire    [575:0] gu_c
);

//
// reciprocal table, as in raster_tex.v: r0[i] = floor((2^32 - 1) / (2^15 + 32 i + 16));
// one read port for each part
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
always @(posedge clk) begin
    ra_q <= rtab[ra_addr];
    rb_q <= rtab[rb_addr];
end

    // float to 8 fraction bits (su_fix8)
    function signed [31:0] fix8(input [31:0] b);
        reg [31:0] m, a;
        begin
            m = {8'd0, 1'b1, b[22:0]};
            if(b[30:23] >= 8'd150) a = 32'h7FFF_FFFF;
            else if(b[30:23] >= 8'd142) a = m << (b[30:23] - 8'd142);
            else if(b[30:23] > 8'd118) a = m >> (8'd142 - b[30:23]);
            else a = 0;
            fix8 = b[31] ? -a : a;
        end
    endfunction

    // bit length of a positive number, and the number shifted to 16 bits (norm16)
    function [5:0] bitlen(input [34:0] v);
        integer i;
        begin
            bitlen = 0;
            for(i = 0; i < 35; i = i + 1) if(v[i]) bitlen = i[5:0] + 6'd1;
        end
    endfunction
    function [15:0] norm16(input [34:0] v, input [5:0] l);
        reg [50:0] w;
        begin
            w = {v, 16'd0} >> l;
            norm16 = w[15:0];
        end
    endfunction
    function [9:0] norm16_10(input [34:0] v, input [5:0] l);  // its table index, bits 14..5
        reg [15:0] n;
        begin
            n = norm16(v, l);
            norm16_10 = n[14:5];
        end
    endfunction

    // saturated to fewer bits
    function signed [23:0] sat24(input signed [31:0] v);
        sat24 = v < -32'sd8388608 ? -24'sd8388608 : v > 32'sd8388607 ? 24'sd8388607 : v[23:0];
    endfunction
    function signed [20:0] sat21(input signed [49:0] v);      // +-(2^20 - 1)
        sat21 = v < -50'sd1048575 ? -21'sd1048575 : v > 50'sd1048575 ? 21'sd1048575 : v[20:0];
    endfunction
    function signed [15:0] sat16(input signed [52:0] v);
        sat16 = v < -53'sd32768 ? -16'sd32768 : v > 53'sd32767 ? 16'sd32767 : v[15:0];
    endfunction
    function signed [26:0] sat27(input signed [41:0] v);
        sat27 = v < -42'sd67108864 ? -27'sd67108864 : v > 42'sd67108863 ? 27'sd67108863 : v[26:0];
    endfunction
    function signed [31:0] sat32(input signed [64:0] v);
        sat32 = v < -65'sd2147483648 ? -32'sd2147483648 : v > 65'sd2147483647 ? 32'sd2147483647 : v[31:0];
    endfunction

//
// registers the CPU writes
//
    reg  signed [15:0] cen_x, cen_y;
    reg  [15:0] scale;
    reg  [12:0] in_mode;
    reg  [31:0] in_tex, in_size, far8;
    reg  signed [31:0] in_r, in_u, in_f, in_s, in_t;
    wire signed [31:0] wr_fix = fix8(wr_data);
    wire        trigger = wr && (wr_addr == 4'd9 || wr_addr == 4'd10) && !reset;

always @(posedge clk) begin
    if(wr && wr_addr == 4'd0) begin cen_x <= wr_data[15:0]; cen_y <= wr_data[31:16]; end
    if(wr && wr_addr == 4'd1) scale <= wr_data[15:0];
    if(wr && wr_addr == 4'd2) in_mode <= wr_data[12:0];
    if(wr && wr_addr == 4'd3) in_tex <= wr_data;
    if(wr && wr_addr == 4'd4) in_size <= wr_data;
    if(wr && wr_addr == 4'd5) in_r <= wr_fix;
    if(wr && wr_addr == 4'd6) in_u <= wr_fix;
    if(wr && wr_addr == 4'd7) in_f <= wr_fix;
    if(wr && wr_addr == 4'd8) in_s <= wr_fix;
    if(wr && wr_addr == 4'd12) far8 <= wr_data;
end

//
// the polygon being written: a corner goes in one clock after its last write; complete,
// the clipper copies it out
//
    reg  [575:0] ib;
    reg  [2:0]  ib_n, ib_cnt;
    reg  [9:0]  ib_mode;
    reg  [31:0] ib_tex, ib_size;
    reg         ib_full = 0, tw = 0, tw_first;
    reg  [9:0]  cl_mode;                        // the polygon in the clipper
    reg  [31:0] cl_tex, cl_size;
    wire        cl_idle, cl_taken, co_valid, co_first;
    wire [143:0] co_c;
    wire        co_take;                        // the corner part takes a corner
    wire [2:0]  tw_idx = tw_first ? 3'd0 : ib_cnt;
    wire [2:0]  tw_n = tw_first ? (in_mode[12:10] < 3'd3 ? 3'd3 : in_mode[12:10] > 3'd4 ? 3'd4 : in_mode[12:10]) : ib_n;

always @(posedge clk) begin
    if(trigger && ready) begin
        in_t <= wr_fix;
        tw <= 1;
        tw_first <= wr_addr == 4'd10;
    end
    if(tw) begin
        tw <= 0;
        ib[144 * tw_idx +: 144] <= {sat24(in_t), sat24(in_s), in_f, in_u, in_r};
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
// corner part
//
    reg         cu_busy = 0, cu_done = 0;
    reg  [4:0]  cu_step;
    reg  signed [31:0] c_r, c_u, c_f;
    reg  signed [23:0] c_s, c_t;
    reg         c_first;
    reg  [9:0]  c_mode;
    reg  [31:0] c_tex, c_size;
    reg  [5:0]  c_l;
    reg  [15:0] c_m;
    reg  [16:0] c_r0;
    reg  [33:0] c_pp;
    reg  [16:0] c_rc;
    reg  signed [20:0] c_px, c_py;
    reg  signed [41:0] c_sr, c_tr;
    reg  signed [15:0] c_x, c_y;

    // its multiplier and the work the result is for
    localparam  [3:0]   C_R1 = 4'd1, C_R2 = 4'd2, C_PX = 4'd3, C_PY = 4'd4, C_SR = 4'd5,
                        C_TR = 4'd6, C_X = 4'd7, C_Y = 4'd8;
    reg  signed [34:0] cm_a;
    reg  signed [17:0] cm_b;
    reg  signed [52:0] cm_p1, cm_p2;
    reg  [3:0]  cm_t0 = 0, cm_t1 = 0, cm_t2 = 0;
    wire signed [49:0] cm_shift = $signed(cm_p2[49:0]) >>> c_l;
    // on whole pixels, the fraction cut off as the game's projection does (0x83420)
    wire signed [52:0] cm_x = $signed({{21{cen_x[15]}}, cen_x, 16'd0}) + cm_p2;
    wire signed [52:0] cm_y = $signed({{21{cen_y[15]}}, cen_y, 16'd0}) - cm_p2;
    wire [15:0] cm_xs = sat16(cm_x >>> 16), cm_ys = sat16(cm_y >>> 16);

    wire        cu_take;                        // the triangle part takes the corner
    wire [15:0] c_norm = norm16({3'd0, c_f}, c_l);

always @(posedge clk) begin
    cm_p1 <= cm_a * cm_b;
    cm_p2 <= cm_p1;
    cm_t1 <= cm_t0;
    cm_t2 <= cm_t1;
    cm_t0 <= 0;

    if(co_take) begin
        c_r <= co_c[31:0];
        c_u <= co_c[63:32];
        c_f <= $signed(co_c[95:64]) < 32'sd1 ? 32'sd1 : $signed(co_c[95:64]);
        c_s <= co_c[119:96];
        c_t <= co_c[143:120];
        c_first <= co_first;
        c_mode <= cl_mode;
        c_tex <= cl_tex;
        c_size <= cl_size;
        cu_busy <= 1;
        cu_step <= 0;
    end

    if(cu_busy) begin
        cu_step <= cu_step + 1'b1;
        case(cu_step)
        5'd0: c_l <= bitlen({3'd0, c_f});
        5'd1: begin
            c_m <= c_norm;
            ra_addr <= c_norm[14:5];
        end
        5'd3: begin
            c_r0 <= ra_q;
            cm_a <= {19'd0, c_m};
            cm_b <= {1'b0, ra_q};
            cm_t0 <= C_R1;
        end
        5'd7: begin
            cm_a <= {1'b0, 34'h2_0000_0000 - c_pp};
            cm_b <= {1'b0, c_r0};
            cm_t0 <= C_R2;
        end
        5'd11: begin cm_a <= {{3{c_r[31]}}, c_r}; cm_b <= {1'b0, c_rc}; cm_t0 <= C_PX; end
        5'd12: begin cm_a <= {{3{c_u[31]}}, c_u}; cm_b <= {1'b0, c_rc}; cm_t0 <= C_PY; end
        5'd13: begin cm_a <= {{11{c_s[23]}}, c_s}; cm_b <= {1'b0, c_rc}; cm_t0 <= C_SR; end
        5'd14: begin cm_a <= {{11{c_t[23]}}, c_t}; cm_b <= {1'b0, c_rc}; cm_t0 <= C_TR; end
        5'd15: begin cm_a <= {{14{c_px[20]}}, c_px}; cm_b <= {2'd0, scale}; cm_t0 <= C_X; end
        5'd16: begin cm_a <= {{14{c_py[20]}}, c_py}; cm_b <= {2'd0, scale}; cm_t0 <= C_Y; end
        5'd20: begin cu_busy <= 0; cu_done <= 1; end
        default: ;
        endcase
    end

    case(cm_t2)
    C_R1: c_pp <= cm_p2[33:0];
    C_R2: c_rc <= cm_p2[48:32];
    C_PX: c_px <= sat21(cm_shift);
    C_PY: c_py <= sat21(cm_shift);
    C_SR: c_sr <= cm_p2[41:0];
    C_TR: c_tr <= cm_p2[41:0];
    C_X:  c_x <= {cm_xs[15:4], 4'd0};
    C_Y:  c_y <= {cm_ys[15:4], 4'd0};
    default: ;
    endcase

    if(cu_take) cu_done <= 0;
    if(reset) begin
        cu_busy <= 0;
        cu_done <= 0;
    end
end

//
// triangle part: corner 0 (the polygon's first), 1 (the one before) and 2 (the new one)
//
    reg  signed [15:0] px0, py0, px1, py1, px2, py2;
    reg  [16:0] prc0, prc1, prc2;
    reg  [5:0]  pl0, pl1, pl2;
    reg  signed [41:0] psr0, psr1, psr2, ptr0, ptr1, ptr2;
    reg  [9:0]  p_mode;
    reg  [31:0] p_tex, p_size;
    reg  [1:0]  p_have = 0;                     // corners of the polygon so far, up to 2
    reg         tu_busy = 0;
    reg  [5:0]  tu_step;

    assign cu_take = cu_done && !tu_busy && !go;
    assign co_take = co_valid && !cu_busy && !cu_done;
    assign ready = !ib_full && !tw;
    assign gu_ready = !ib_full && !tw;
    assign busy = tw || ib_full || !cl_idle || cu_busy || cu_done || tu_busy || go;

    // per triangle
    reg  signed [16:0] dx1, dy1, dx2, dy2;
    reg  [5:0]  d0, d1, d2;                     // l - the smallest l
    reg  signed [24:0] q0, q1, q2;
    reg  signed [26:0] uv [0:5];                // U0, U1, U2, V0, V1, V2
    reg  signed [27:0] da1 [0:2];               // Q, U, V at corner 1 minus at corner 0
    reg  signed [27:0] da2 [0:2];
    reg  signed [17:0] fx, fy;
    reg  signed [34:0] td1, td;
    reg  [5:0]  ld;
    reg  [34:0] tad;                            // |d|
    reg  signed [64:0] t_rnd;                   // 2^(ld + 3), added to a plane's products
    reg  [15:0] md;
    reg  [16:0] tr0;
    reg  [33:0] tpp;
    reg  [16:0] trcd;
    reg  signed [45:0] tnx [0:2];
    reg  signed [45:0] tny [0:2];
    reg  signed [31:0] tgx [0:2];
    reg  signed [31:0] tgy [0:2];
    reg  signed [47:0] ta [0:2];

    // the U, V shifter: one value a clock
    reg  [2:0]  sh_i;
    reg  signed [41:0] sh_v;
    reg  [5:0]  sh_d;
    wire signed [41:0] sh_out = sh_v >>> (6'd10 + sh_d);

    // its multiplier
    localparam  [3:0]   T_D1 = 4'd1, T_D2 = 4'd2, T_R1 = 4'd3, T_R2 = 4'd4, T_NX1 = 4'd5,
                        T_NX2 = 4'd6, T_NY1 = 4'd7, T_NY2 = 4'd8, T_GX = 4'd9, T_GY = 4'd10,
                        T_A1 = 4'd11, T_A2 = 4'd12;
    reg  signed [46:0] tm_a;
    reg  signed [17:0] tm_b;
    reg  signed [64:0] tm_p1, tm_p2;
    reg  [3:0]  tm_t0 = 0, tm_t1 = 0, tm_t2 = 0;
    reg  [1:0]  tm_k0, tm_k1, tm_k2;
    wire signed [64:0] tm_grad = tm_p2 >>> (ld + 6'd4);         // the rounding is in tm_p2
    wire signed [49:0] tm_asum = $signed({{2{ta[tm_k2][47]}}, ta[tm_k2]}) + $signed(tm_p2[49:0]);
    wire signed [31:0] tm_a0 = tm_k2 == 2'd0 ? {{7{q0[24]}}, q0} :          // Q, U, V at corner 0
                               tm_k2 == 2'd1 ? {{5{uv[0][26]}}, uv[0]} : {{5{uv[3][26]}}, uv[3]};
    wire signed [49:0] tm_a2 = $signed({{18{tm_a0[31]}}, tm_a0}) + (tm_asum >>> 12);

    task issue(input signed [46:0] a, input signed [17:0] b, input [3:0] t, input [1:0] k);
        begin
            tm_a <= a;
            tm_b <= b;
            tm_t0 <= t;
            tm_k0 <= k;
        end
    endtask

    function signed [46:0] neg_if(input signed [45:0] v);
        neg_if = td[34] ? -{v[45], v} : {v[45], v};
    endfunction

    reg  signed [9:0] xr, yr;                   // reference pixel

    // the operands at the multiplier's widths
    wire signed [17:0] bx1 = {dx1[16], dx1}, by1 = {dy1[16], dy1}, bx2 = {dx2[16], dx2}, by2 = {dy2[16], dy2};
    function signed [46:0] a28(input signed [27:0] v);
        a28 = {{19{v[27]}}, v};
    endfunction
    function signed [46:0] a32(input signed [31:0] v);
        a32 = {{15{v[31]}}, v};
    endfunction

always @(posedge clk) begin
    tm_p1 <= tm_a * tm_b;
    tm_p2 <= tm_p1 + (tm_t1 == T_GX || tm_t1 == T_GY ? t_rnd : tm_t1 == T_A2 ? 65'sd2048 : 65'sd0);
    tm_t1 <= tm_t0;
    tm_t2 <= tm_t1;
    tm_k1 <= tm_k0;
    tm_k2 <= tm_k1;
    tm_t0 <= 0;

    if(!cl_idle || cu_busy || tu_busy) busy_clocks <= busy_clocks + 1'b1;
    if(wr && wr_addr == 4'd11) busy_clocks <= 0;

    // a corner from the corner part
    if(cu_take) begin
        if(c_first || p_have == 2'd0) begin
            px0 <= c_x; py0 <= c_y; prc0 <= c_rc; pl0 <= c_l; psr0 <= c_sr; ptr0 <= c_tr;
            p_mode <= c_mode; p_tex <= c_tex; p_size <= c_size;
            p_have <= c_first ? 2'd1 : 2'd0;    // a polygon begins with a first corner
        end else if(p_have == 2'd1) begin
            px1 <= c_x; py1 <= c_y; prc1 <= c_rc; pl1 <= c_l; psr1 <= c_sr; ptr1 <= c_tr;
            p_have <= 2'd2;
        end else begin
            px2 <= c_x; py2 <= c_y; prc2 <= c_rc; pl2 <= c_l; psr2 <= c_sr; ptr2 <= c_tr;
            tu_busy <= 1;
            tu_step <= 0;
        end
    end

    // the triangle: positions for every triangle, the planes for a textured one
    if(tu_busy) begin
        tu_step <= tu_step + 1'b1;
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
            dx1 <= px1 - px0; dy1 <= py1 - py0;
            dx2 <= px2 - px0; dy2 <= py2 - py0;
            begin : lmin_block
                reg [5:0] lm;
                lm = pl0 < pl1 ? pl0 : pl1;
                if(pl2 < lm) lm = pl2;
                d0 <= pl0 - lm; d1 <= pl1 - lm; d2 <= pl2 - lm;
                q0 <= {2'd0, prc0, 6'd0} >> (pl0 - lm);
                q1 <= {2'd0, prc1, 6'd0} >> (pl1 - lm);
                q2 <= {2'd0, prc2, 6'd0} >> (pl2 - lm);
            end
            xr <= px0[15] ? 10'sd0 : px0[15:4] > 12'd319 ? 10'sd319 : {1'b0, px0[12:4]};
            yr <= py0[15] ? 10'sd0 : py0[15:4] > 12'd199 ? 10'sd199 : {1'b0, py0[12:4]};
            sh_i <= 0;
            sh_v <= psr0;
            sh_d <= pl0 - (pl0 < pl1 ? (pl0 < pl2 ? pl0 : pl2) : (pl1 < pl2 ? pl1 : pl2));
        end
        6'd1: begin
            issue({{30{dx1[16]}}, dx1}, by2, T_D1, 0);
            fx <= $signed({5'd0, xr[8:0], 4'd8}) - {{2{px0[15]}}, px0};
            fy <= $signed({5'd0, yr[8:0], 4'd8}) - {{2{py0[15]}}, py0};
            t_ref <= {{6{yr[9]}}, yr, {6{xr[9]}}, xr};
        end
        6'd2: issue({{30{dx2[16]}}, dx2}, by1, T_D2, 0);
        6'd6: begin                             // |d| and its bit length
            if(td == 0) tu_busy <= 0;           // no area: left out
            tad <= td[34] ? -td : td;
            ld <= bitlen(td[34] ? -td : td);
        end
        6'd7: begin                             // 16 bits of |d|, the rounding of the planes
            md <= norm16(tad, ld);
            rb_addr <= norm16_10(tad, ld);
            t_rnd <= 65'sd1 <<< (ld + 6'd3);
            da1[0] <= {{3{q1[24]}}, q1} - {{3{q0[24]}}, q0}; da2[0] <= {{3{q2[24]}}, q2} - {{3{q0[24]}}, q0};
            da1[1] <= uv[1] - uv[0]; da2[1] <= uv[2] - uv[0];
            da1[2] <= uv[4] - uv[3]; da2[2] <= uv[5] - uv[3];
        end
        6'd9: begin tr0 <= rb_q; issue({31'd0, md}, {1'b0, rb_q}, T_R1, 0); end
        6'd10:  issue(a28(da1[0]), by2, T_NX1, 0);
        6'd11: issue(a28(da2[0]), by1, T_NX2, 0);
        6'd12: issue(a28(da2[0]), bx1, T_NY1, 0);
        6'd13: issue({13'd0, 34'h2_0000_0000 - tpp}, {1'b0, tr0}, T_R2, 0);
        6'd14: issue(a28(da1[0]), bx2, T_NY2, 0);
        6'd15: issue(a28(da1[1]), by2, T_NX1, 1);
        6'd16: issue(a28(da2[1]), by1, T_NX2, 1);
        6'd17: issue(neg_if(tnx[0]), {1'b0, trcd}, T_GX, 0);
        6'd18: issue(neg_if(tny[0]), {1'b0, trcd}, T_GY, 0);
        6'd19: issue(a28(da2[1]), bx1, T_NY1, 1);
        6'd20: issue(a28(da1[1]), bx2, T_NY2, 1);
        6'd21: issue(a28(da1[2]), by2, T_NX1, 2);
        6'd22: issue(a28(da2[2]), by1, T_NX2, 2);
        6'd23: issue(neg_if(tnx[1]), {1'b0, trcd}, T_GX, 1);
        6'd24: issue(a32(tgx[0]), fx, T_A1, 0);
        6'd25: issue(a32(tgy[0]), fy, T_A2, 0);
        6'd26: issue(neg_if(tny[1]), {1'b0, trcd}, T_GY, 1);
        6'd27: issue(a28(da2[2]), bx1, T_NY1, 2);
        6'd28: issue(a28(da1[2]), bx2, T_NY2, 2);
        6'd29: issue(neg_if(tnx[2]), {1'b0, trcd}, T_GX, 2);
        6'd30: issue(a32(tgx[1]), fx, T_A1, 1);
        6'd31: issue(a32(tgy[1]), fy, T_A2, 1);
        6'd32: issue(neg_if(tny[2]), {1'b0, trcd}, T_GY, 2);
        6'd33: issue(a32(tgx[2]), fx, T_A1, 2);
        6'd36: issue(a32(tgy[2]), fy, T_A2, 2);
        6'd40: begin tu_busy <= 0; go <= 1; end
        default: ;
        endcase

        // U0, U1, U2, V0, V1, V2 through the shifter, one a clock from step 1
        if(tu_step >= 6'd1 && tu_step <= 6'd6) begin
            uv[sh_i] <= sat27(sh_out);
            sh_i <= sh_i + 1'b1;
            case(sh_i)
            3'd0: begin sh_v <= psr1; sh_d <= d1; end
            3'd1: begin sh_v <= psr2; sh_d <= d2; end
            3'd2: begin sh_v <= ptr0; sh_d <= d0; end
            3'd3: begin sh_v <= ptr1; sh_d <= d1; end
            3'd4: begin sh_v <= ptr2; sh_d <= d2; end
            default: ;
            endcase
        end
    end

    case(tm_t2)
    T_D1:  td1 <= tm_p2[34:0];
    T_D2:  td <= td1 - tm_p2[34:0];
    T_R1:  tpp <= tm_p2[33:0];
    T_R2:  trcd <= tm_p2[48:32];
    T_NX1: tnx[tm_k2] <= tm_p2[45:0];
    T_NX2: tnx[tm_k2] <= tnx[tm_k2] - tm_p2[45:0];
    T_NY1: tny[tm_k2] <= tm_p2[45:0];
    T_NY2: tny[tm_k2] <= tny[tm_k2] - tm_p2[45:0];
    T_GX: begin
        tgx[tm_k2] <= sat32(tm_grad);
        case(tm_k2)
        2'd0: t_dx0 <= sat32(tm_grad);
        2'd1: t_dx1 <= sat32(tm_grad);
        default: t_dx2 <= sat32(tm_grad);
        endcase
    end
    T_GY: begin
        tgy[tm_k2] <= sat32(tm_grad);
        case(tm_k2)
        2'd0: t_dy0 <= sat32(tm_grad);
        2'd1: t_dy1 <= sat32(tm_grad);
        default: t_dy2 <= sat32(tm_grad);
        endcase
    end
    T_A1:  ta[tm_k2] <= tm_p2[47:0];
    T_A2: begin
        case(tm_k2)
        2'd0: t_a0 <= sat32({{15{tm_a2[49]}}, tm_a2});
        2'd1: t_a1 <= sat32({{15{tm_a2[49]}}, tm_a2});
        default: t_a2 <= sat32({{15{tm_a2[49]}}, tm_a2});
        endcase
    end
    default: ;
    endcase

    // the rasterizer takes the triangle; the new corner is the next triangle's corner 1
    if(go && rs_accept) go <= 0;
    if((go && rs_accept) || (tu_busy && tu_step == 6'd6 && td == 0)) begin
        px1 <= px2; py1 <= py2; prc1 <= prc2; pl1 <= pl2; psr1 <= psr2; ptr1 <= ptr2;
    end

    if(reset) begin
        tu_busy <= 0;
        go <= 0;
        p_have <= 0;
    end
end

endmodule
