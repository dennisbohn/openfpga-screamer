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
// one at a time. A corner takes two clocks a pass, a new point about 25 more (one
// multiplier, operands registered, the product three clocks after it was issued).
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
    reg  [9:0]  r_addr;
    reg  [16:0] r_q;
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

    function [5:0] bitlen(input [37:0] v);
        integer i;
        begin
            bitlen = 0;
            for(i = 0; i < 38; i = i + 1) if(v[i]) bitlen = i[5:0] + 6'd1;
        end
    endfunction

    function [15:0] norm16(input [37:0] v, input [5:0] l);
        reg [53:0] w;
        begin
            w = {v, 16'd0} >> l;
            norm16 = w[15:0];
        end
    endfunction

//
// the buffer: two halves of 16 corners, read at {cur, ridx}, written at {!cur, m}
//
    reg  [143:0] buf_c [0:31];
    reg          cur;
    reg  [3:0]   ridx, m;
    reg          we;
    reg  [4:0]   wa;
    reg  [143:0] wd;
    wire [143:0] buf_q = buf_c[{cur, ridx}];
always @(posedge clk) if(we) buf_c[wa] <= wd;

    localparam  [3:0]   L_IDLE = 4'd0, L_LOAD = 4'd1, L_DECIDE = 4'd2, L_PSTART = 4'd3,
                        L_READ = 4'd4, L_EDGE = 4'd5, L_X = 4'd6, L_PEND = 4'd7, L_OUT = 4'd8;
    reg  [3:0]   state = L_IDLE;
    reg  [3:0]   np;                            // corners of the polygon now
    reg  [2:0]   k;                             // the plane of this pass
    reg  [5:0]   any, all;
    reg  [3:0]   i;                             // edge i: corners i and i + 1
    reg  [143:0] a, b, first;
    reg  signed [36:0] da, db;
    reg  [4:0]   xs;                            // step of the new point

    assign idle = state == L_IDLE;
    assign out_c = buf_q;
    assign out_first = ridx == 4'd0;

    wire [143:0] in_k = in_c[144 * i +: 144];
    wire [143:0] b_now = i == np - 4'd1 ? first : buf_q;
    wire [5:0]   code = outcode(in_k, far8);
    wire [2:0]   next_k = plane_from(any, k + 3'd1);

    // the new point
    reg  [36:0]  ea, eb;
    reg  [37:0]  xd;
    reg  [5:0]   xl;
    reg  [15:0]  xm;
    wire [15:0]  xm_now = norm16(xd, xl);
    reg  [16:0]  xr0, xrc;
    reg  [33:0]  xp;
    reg  [16:0]  t16;
    reg  [143:0] xn;
    reg  signed [37:0] mul_a;
    reg  signed [17:0] mul_b;
    reg  signed [55:0] mul_p1, mul_p2;
    reg  [2:0]   mul_t0 = 0, mul_t1 = 0, mul_t2 = 0;
    localparam  [2:0]   X_R1 = 3'd1, X_R2 = 3'd2, X_T = 3'd3, X_LR = 3'd4, X_LU = 3'd5,
                        X_LF = 3'd6, X_LS = 3'd7;
    reg          lt;                            // the fifth product (t) after X_LS
    reg          lt1, lt2;
    wire signed [55:0] mul_t16 = mul_p2 >>> xl;
    wire signed [39:0] lerp_d = mul_p2[55:16];  // (b - a) t >> 16

    function signed [37:0] diff32(input [31:0] bv, input [31:0] av);
        diff32 = {{6{bv[31]}}, bv} - {{6{av[31]}}, av};
    endfunction
    function signed [37:0] diff24(input [23:0] bv, input [23:0] av);
        diff24 = {{14{bv[23]}}, bv} - {{14{av[23]}}, av};
    endfunction

always @(posedge clk) begin
    taken <= 0;
    we <= 0;
    mul_p1 <= mul_a * mul_b;
    mul_p2 <= mul_p1;
    mul_t1 <= mul_t0;
    mul_t2 <= mul_t1;
    lt1 <= lt;
    lt2 <= lt1;
    mul_t0 <= 0;
    lt <= 0;

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
        wd <= in_k;
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
        if(all != 0) state <= L_IDLE;           // wholly outside one plane
        else if(any == 0) begin out_valid <= 1; state <= L_OUT; end
        else begin
            k <= plane_from(any, 3'd0);
            state <= L_PSTART;
        end
    end

    L_PSTART: begin                             // corner 0 is the first edge's a
        a <= buf_q;
        first <= buf_q;
        da <= pdist(buf_q, k, far8);
        i <= 0;
        m <= 0;
        ridx <= 1;
        state <= L_READ;
    end

    L_READ: begin                               // edge i: its b and the distance of b
        b <= b_now;
        db <= pdist(b_now, k, far8);
        state <= L_EDGE;
    end

    L_EDGE: begin                               // a inside goes on; an edge across adds a point
        if(!da[36]) begin
            we <= 1;
            wa <= {!cur, m};
            wd <= a;
        end
        m <= m + {3'd0, !da[36]};
        if(da[36] != db[36]) begin
            xs <= 0;
            state <= L_X;
        end else begin
            a <= b;
            da <= db;
            i <= i + 1'b1;
            ridx <= i + 4'd2;
            state <= i == np - 4'd1 ? L_PEND : L_READ;
        end
    end

    L_X: begin                                  // the point where a-b crosses the plane
        xs <= xs + 1'b1;
        case(xs)
        5'd0: begin
            ea <= da[36] ? -da : da;
            eb <= db[36] ? -db : db;
        end
        5'd1: xd <= {1'b0, ea} + {1'b0, eb};
        5'd2: xl <= bitlen(xd);
        5'd3: begin
            xm <= xm_now;
            r_addr <= xm_now[14:5];
        end
        5'd5: begin
            xr0 <= r_q;
            mul_a <= {22'd0, xm};
            mul_b <= {1'b0, r_q};
            mul_t0 <= X_R1;
        end
        5'd9: begin
            mul_a <= {4'd0, 34'h2_0000_0000 - xp};
            mul_b <= {1'b0, xr0};
            mul_t0 <= X_R2;
        end
        5'd13: begin mul_a <= {1'b0, ea}; mul_b <= {1'b0, xrc}; mul_t0 <= X_T; end
        5'd17: begin mul_a <= diff32(b[31:0], a[31:0]); mul_b <= {1'b0, t16}; mul_t0 <= X_LR; end
        5'd18: begin mul_a <= diff32(b[63:32], a[63:32]); mul_b <= {1'b0, t16}; mul_t0 <= X_LU; end
        5'd19: begin mul_a <= diff32(b[95:64], a[95:64]); mul_b <= {1'b0, t16}; mul_t0 <= X_LF; end
        5'd20: begin mul_a <= diff24(b[119:96], a[119:96]); mul_b <= {1'b0, t16}; mul_t0 <= X_LS; end
        5'd21: begin mul_a <= diff24(b[143:120], a[143:120]); mul_b <= {1'b0, t16}; lt <= 1; end
        5'd25: begin                            // the new point in, on to the next edge
            we <= 1;
            wa <= {!cur, m};
            wd <= xn;
            m <= m + 1'b1;
            a <= b;
            da <= db;
            i <= i + 1'b1;
            ridx <= i + 4'd2;
            state <= i == np - 4'd1 ? L_PEND : L_READ;
        end
        default: ;
        endcase
    end

    L_PEND: begin                               // the pass is done: its corners are the polygon
        np <= m;
        cur <= !cur;
        ridx <= 0;
        if(m < 4'd3) state <= L_IDLE;
        else if(next_k == 3'd6) begin out_valid <= 1; state <= L_OUT; end
        else begin
            k <= next_k;
            state <= L_PSTART;
        end
    end

    L_OUT: if(out_take) begin                   // corner ridx taken
        ridx <= ridx + 1'b1;
        if(ridx == np - 4'd1) begin
            out_valid <= 0;
            state <= L_IDLE;
        end
    end

    default: state <= L_IDLE;
    endcase

    case(mul_t2)
    X_R1: xp <= mul_p2[33:0];
    X_R2: xrc <= mul_p2[48:32];
    X_T:  t16 <= mul_t16 > 56'sd65536 ? 17'd65536 : mul_t16[16:0];
    X_LR: xn[31:0] <= a[31:0] + lerp_d[31:0];
    X_LU: xn[63:32] <= a[63:32] + lerp_d[31:0];
    X_LF: xn[95:64] <= a[95:64] + lerp_d[31:0];
    X_LS: xn[119:96] <= a[119:96] + lerp_d[23:0];
    default: ;
    endcase
    if(lt2) xn[143:120] <= a[143:120] + lerp_d[23:0];

    if(reset) begin
        state <= L_IDLE;
        out_valid <= 0;
    end
end

endmodule
