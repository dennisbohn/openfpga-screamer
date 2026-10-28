//
// Clipper, in front of the setup unit's corner part (setup.v): a polygon of 3 or 4 corners
// in view space (8 fraction bits) cut at the planes it leaves, bit for bit su_clip in
// firmware/setup.c:
//   near f >= 128, guard |r|, |u| <= 14 f, far f <= far8 (the planes 0..5); dropped when all
//   corners are outside one plane; else one pass for each plane a corner is outside of, in
//   that order, round the polygon: a corner inside goes on, an edge across the plane adds
//   the point at t = |da| / (|da| + |db|) with 16 fraction bits (the reciprocal table and one
//   Newton step), each value a + ((b - a) t >> 16).
// The polygon is copied in (one clock a corner, then the input is free again) and cut in a
// buffer of two halves, one read and the other written in a pass; its corners then go on
// one at a time. The buffer is block RAM, read one clock after its address is set. A
// corner takes three clocks a pass, a new point about 25 more (one multiplier, operands
// registered, the product three clocks after it was issued).
// Of an edge only b is kept in a register: corner a is b of the edge before, written on
// (if inside) as b is replaced, and for a new point the buffer reads it again; the new
// point is put together in the buffer's write register. The wide registers are written
// through explicit enables and small selects (the build does not restructure multiplexers,
// so a register held in most states of a case would become a wide mux of the state).
//
// A corner: {t[23:0], s[23:0], f, u, r} = 144 bits.
//

module clipper (
    input   wire            clk,
    input   wire            reset,

    input   wire            start,              // a polygon at in_c, taken while idle
    input   wire    [2:0]   n,                  // its corners, 3 or 4
    input   wire    [575:0] in_c,               // corner k at [144 k +: 144]
    input   wire    [31:0]  far8,
    output  wire            idle,
    output  reg             taken = 0,          // one clock: in_c is free again

    output  reg             out_valid = 0,
    output  wire            out_first,
    output  wire    [143:0] out_c,
    input   wire            out_take
);

//
// reciprocal table, as in raster_tex.v
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
    reg  [9:0]  r_addr;                         // set at step 3 of a new point only, so r_q
    reg  [16:0] r_q;                            // holds its entry from step 5 on
always @(posedge clk) r_q <= rtab[r_addr];

    // distance from plane k, >= 0 inside
    function signed [36:0] pdist(input [143:0] c, input [2:0] k, input [31:0] far);
        reg signed [36:0] r, u, f, g;
        begin
            r = {{5{c[31]}}, c[31:0]};
            u = {{5{c[63]}}, c[63:32]};
            f = {{5{c[95]}}, c[95:64]};
            g = (f <<< 4) - (f <<< 1);          // 14 f
            case(k)
            3'd0: pdist = f - 37'sd32768;
            3'd1: pdist = g - r;
            3'd2: pdist = g + r;
            3'd3: pdist = g - u;
            3'd4: pdist = g + u;
            default: pdist = {{5{far[31]}}, far} - f;
            endcase
        end
    endfunction

    function [5:0] outcode(input [143:0] c, input [31:0] far);
        integer k;
        reg signed [36:0] d;
        begin
            for(k = 0; k < 6; k = k + 1) begin
                d = pdist(c, k[2:0], far);
                outcode[k] = d[36];
            end
        end
    endfunction

    // the first plane of mask from plane k on, 6 for none
    function [2:0] plane_from(input [5:0] mask, input [2:0] k);
        integer j;
        begin
            plane_from = 3'd6;
            for(j = 5; j >= 0; j = j - 1) if(mask[j] && j[2:0] >= k) plane_from = j[2:0];
        end
    endfunction

    function [5:0] bitlen(input [35:0] v);
        integer i;
        begin
            bitlen = 0;
            for(i = 0; i < 36; i = i + 1) if(v[i]) bitlen = i[5:0] + 6'd1;
        end
    endfunction

//
// the buffer: two halves of 16 corners, read at {cur, ridx} (buf_q a clock later), written
// at {!cur, m}
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [143:0] buf_c [0:31];
    reg          cur;
    reg  [3:0]   ridx, m;
    reg          we;
    reg  [4:0]   wa;
    reg  [143:0] wd;
    reg  [143:0] buf_q;
always @(posedge clk) begin
    if(we) buf_c[wa] <= wd;
    buf_q <= buf_c[{cur, ridx}];
end

    localparam  [3:0]   L_IDLE = 4'd0, L_LOAD = 4'd1, L_DECIDE = 4'd2, L_PSTART = 4'd3,
                        L_READ = 4'd4, L_EDGE = 4'd5, L_X = 4'd6, L_PEND = 4'd7, L_OUT = 4'd8,
                        L_WAIT = 4'd9;          // the buffer reads the new address, then ret
    reg  [3:0]   state = L_IDLE, ret;
    reg  [3:0]   np;                            // corners of the polygon now
    reg  [2:0]   k;                             // the plane of this pass
    reg  [5:0]   any, all;
    reg  [3:0]   i;                             // edge i: corners i and i + 1
    reg  [143:0] b;                             // corner i + 1 (until L_READ: corner i)
    reg  signed [36:0] da, db;                  // the distances of corners i and i + 1
    reg  [4:0]   xs;                            // step of the new point

    assign idle = state == L_IDLE;
    assign out_c = buf_q;
    assign out_first = ridx == 4'd0;

    // corner i of the input (n is 3 or 4, so i < 4 while loading)
    wire [143:0] in_k = in_c[144 * i[1:0] +: 144];
    wire [5:0]   code = outcode(in_k, far8);
    wire [2:0]   next_k = plane_from(any, k + 3'd1);

    wire         s_load = state == L_LOAD;
    wire         s_pstart = state == L_PSTART;
    wire         s_read = state == L_READ;
    wire         s_x = state == L_X;
    wire         across = da[36] != db[36];
    // the next edge's b: the corner after it, corner 0 for the last edge (the half being
    // read is not written in a pass, so corner 0 is still there)
    wire [3:0]   i2 = i + 4'd2;
    wire [3:0]   ridx_next = i2 == np ? 4'd0 : i2;

    // pdist(buf_q, k) with one adder: x + y or x - y
    wire [36:0]  q_r = {{5{buf_q[31]}}, buf_q[31:0]};
    wire [36:0]  q_u = {{5{buf_q[63]}}, buf_q[63:32]};
    wire [36:0]  q_f = {{5{buf_q[95]}}, buf_q[95:64]};
    wire [36:0]  q_g = (q_f << 4) - (q_f << 1);
    wire [36:0]  pd_x = k == 3'd0 ? q_f : k >= 3'd5 ? {{5{far8[31]}}, far8} : q_g;
    wire [36:0]  pd_y = k == 3'd0 ? 37'd32768 : k <= 3'd2 ? q_r : k <= 3'd4 ? q_u : q_f;
    wire         pd_neg = k[0] || k == 3'd0 || k >= 3'd5;
    wire [36:0]  pd = pd_x + (pd_y ^ {37{pd_neg}}) + {36'd0, pd_neg};

    // the new point (corner i in buf_q from step 1 on); the fields being 32 bits, a distance
    // is at most 14 |f| + |r| <= 15 x 2^31 < 2^35, so xd has 36 bits
    wire [37:0]  xd_now = da[36] ? {db[36], db} - {da[36], da} : {da[36], da} - {db[36], db};
    reg  [35:0]  xd;                            // |da| + |db|, the signs differing
    reg  [5:0]   xl;
    reg  [33:0]  xp;                            // m (from step 3), then 2^33 - m r0
    reg  signed [17:0] xrc;                     // rc, negated if da < 0
    reg  [16:0]  t16;
    reg  signed [37:0] mul_a;
    reg  signed [17:0] mul_b;
    reg  signed [55:0] mul_p1, mul_p2;
    reg  [2:0]   mul_t0 = 0, mul_t1 = 0, mul_t2 = 0;
    localparam  [2:0]   X_R1 = 3'd1, X_R2 = 3'd2, X_T = 3'd3, X_LR = 3'd4, X_LU = 3'd5,
                        X_LF = 3'd6, X_LS = 3'd7;
    reg          lt;                            // the fifth product (t) after X_LS
    reg          lt1, lt2;
    wire signed [39:0] lerp_d = mul_p2[55:16];  // (b - a) t >> 16
    // one shifter right by xl (at most 36) for norm16 at step 3 and for t at X_T: t is
    // |da| rc >> xl, below 2^17 as |da| <= xd < 2^xl and rc < 2^17, so the product has 53
    // bits, its low 17 bits shifted are all of t and above 65536 is bit 16 with any below
    wire [52:0]  sh_in = s_x && xs == 5'd3 ? {1'b0, xd, 16'd0} : mul_p2[52:0];
    wire [52:0]  sh = sh_in >> xl;
    wire [15:0]  xm_now = sh[15:0];             // norm16(xd, xl)
    wire [16:0]  t_sh = sh[16:0];

    // a value of the new point: the field of a (buf_q) the product is for, plus it
    reg  [31:0]  xa;
always @* begin
    case(mul_t2)
    X_LR: xa = buf_q[31:0];
    X_LU: xa = buf_q[63:32];
    X_LF: xa = buf_q[95:64];
    X_LS: xa = {8'd0, buf_q[119:96]};
    default: xa = {8'd0, buf_q[143:120]};
    endcase
end
    wire [31:0]  xv = xa + lerp_d[31:0];

    // the multiplier's work this clock (steps 5, 9, 13, 17..21 of a new point)
    reg  [2:0]   op;
    reg          op_lt;
always @* begin
    op = 3'd0;
    op_lt = 1'b0;
    if(s_x) case(xs)
    5'd5:  op = X_R1;
    5'd9:  op = X_R2;
    5'd13: op = X_T;
    5'd17: op = X_LR;
    5'd18: op = X_LU;
    5'd19: op = X_LF;
    5'd20: op = X_LS;
    5'd21: op_lt = 1'b1;
    default: ;
    endcase
end

    // the field of a (buf_q) and b for the lerp at step 17..21 (xs[2:0] 1..5: r, u, f, s, t)
    reg  [31:0]  fa, fb;
always @* begin
    case(xs[2:0])
    3'd1: begin fa = buf_q[31:0]; fb = b[31:0]; end
    3'd2: begin fa = buf_q[63:32]; fb = b[63:32]; end
    3'd3: begin fa = buf_q[95:64]; fb = b[95:64]; end
    3'd4: begin fa = {{8{buf_q[119]}}, buf_q[119:96]}; fb = {{8{b[119]}}, b[119:96]}; end
    default: begin fa = {{8{buf_q[143]}}, buf_q[143:120]}; fb = {{8{b[143]}}, b[143:120]}; end
    endcase
end
    wire [32:0]  fd = {fb[31], fb} - {fa[31], fa};

//
// control
//
always @(posedge clk) begin
    taken <= 0;
    we <= 0;

    case(state)
    L_IDLE: if(start) begin
        np <= {1'b0, n};
        i <= 0;
        any <= 0;
        all <= 6'h3F;
        cur <= 0;
        state <= L_LOAD;
    end

    L_LOAD: begin                               // corner i into the first half, its outcode
        we <= 1;
        wa <= {1'b0, i};
        any <= any | code;
        all <= all & code;
        i <= i + 1'b1;
        if(i == np - 4'd1) begin
            taken <= 1;
            state <= L_DECIDE;
        end
    end

    L_DECIDE: begin
        ridx <= 0;
        state <= L_WAIT;
        if(all != 0) state <= L_IDLE;           // wholly outside one plane
        else if(any == 0) ret <= L_OUT;
        else begin
            k <= plane_from(any, 3'd0);
            ret <= L_PSTART;
        end
    end

    L_WAIT: begin
        if(ret == L_OUT) out_valid <= 1;
        state <= ret;
    end

    L_PSTART: begin                             // corner 0 into b: the first edge's a
        i <= 0;
        m <= 0;
        ridx <= 1;
        ret <= L_READ;
        state <= L_WAIT;
    end

    L_READ: begin                               // edge i: a (in b) goes on if inside, b read
        if(!db[36]) begin
            we <= 1;
            wa <= {!cur, m};
        end
        m <= m + {3'd0, !db[36]};
        state <= L_EDGE;
    end

    L_EDGE: begin                               // an edge across adds a point
        if(across) begin
            xs <= 0;
            ridx <= i;                          // a again, for the new point
            state <= L_X;
        end else begin
            i <= i + 1'b1;
            ridx <= ridx_next;
            ret <= L_READ;
            state <= i == np - 4'd1 ? L_PEND : L_WAIT;
        end
    end

    L_X: begin                                  // the point where a-b crosses the plane
        xs <= xs + 1'b1;
        if(xs == 5'd25) begin                   // the new point in, on to the next edge
            we <= 1;
            wa <= {!cur, m};
            m <= m + 1'b1;
            i <= i + 1'b1;
            ridx <= ridx_next;
            ret <= L_READ;
            state <= i == np - 4'd1 ? L_PEND : L_WAIT;
        end
    end

    L_PEND: begin                               // the pass is done: its corners are the polygon
        np <= m;
        cur <= !cur;
        ridx <= 0;
        state <= L_WAIT;
        if(m < 4'd3) state <= L_IDLE;
        else if(next_k == 3'd6) ret <= L_OUT;
        else begin
            k <= next_k;
            ret <= L_PSTART;
        end
    end

    L_OUT: if(out_take) begin                   // corner ridx taken, the next one read
        ridx <= ridx + 1'b1;
        out_valid <= 0;
        ret <= L_OUT;
        state <= ridx == np - 4'd1 ? L_IDLE : L_WAIT;
    end

    default: state <= L_IDLE;
    endcase

    if(reset) begin
        state <= L_IDLE;
        out_valid <= 0;
    end
end

//
// corner b and the distances
//
always @(posedge clk) begin
    if(s_pstart || s_read) begin
        b <= buf_q;
        db <= pd;
    end
    if(s_read) da <= db;
end

//
// the new point: t, then the five values; what we writes
//
always @(posedge clk) begin
    if(s_x && xs == 5'd1) xd <= xd_now[35:0];
    if(s_x && xs == 5'd2) xl <= bitlen(xd);
    if(s_x && xs == 5'd3) begin
        xp <= {18'd0, xm_now};
        r_addr <= xm_now[14:5];
    end

    // operands: at step 5 m x r0, 9 (2^33 - m r0) x r0, 13 da x (da < 0 ? -rc : rc), that
    // is |da| rc, 17..21 (b - a) x t; the other clocks' products are not used
    mul_a <= xs[4] ? {{5{fd[32]}}, fd} : xs[3] && xs[2] ? {da[36], da} : {4'd0, xp};
    mul_b <= xs[4] ? {1'b0, t16} : xs[3] && xs[2] ? xrc : {1'b0, r_q};
    mul_t0 <= op;
    lt <= op_lt;
    mul_p1 <= mul_a * mul_b;
    mul_p2 <= mul_p1;
    mul_t1 <= mul_t0;
    mul_t2 <= mul_t1;
    lt1 <= lt;
    lt2 <= lt1;

    // the input corner, a (in b until L_READ), or the new point, made here field by field
    if(s_load || s_read) wd <= s_load ? in_k : b;
    case(mul_t2)
    X_R1: xp <= 34'h2_0000_0000 - mul_p2[33:0];
    X_R2: xrc <= da[36] ? -{1'b0, mul_p2[48:32]} : {1'b0, mul_p2[48:32]};
    X_T:  t16 <= t_sh[16] && t_sh[15:0] != 16'd0 ? 17'd65536 : t_sh;
    X_LR: wd[31:0] <= xv;
    X_LU: wd[63:32] <= xv;
    X_LF: wd[95:64] <= xv;
    X_LS: wd[119:96] <= xv[23:0];
    default: ;
    endcase
    if(lt2) wd[143:120] <= xv[23:0];
end

endmodule
