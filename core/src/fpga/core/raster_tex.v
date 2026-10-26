//
// The rasterizer's texture path: one pixel a clock through a pipeline, the same result as
// the C model firmware/raster.c (span_texfx). Per pixel, from acc = Q, U, V with 8
// fraction bits:
//   S1  Q = acc_Q >> 8 limited to [1, 2^31 - 1]; U, V = acc >> 8 limited to 27 bits signed
//   S2  e = bit length of Q - 16, m = Q shifted to 16 bits
//   S3  table address m[14:5]                  S4  p = m r0, r0 = floor((2^32-1) / (2^15 + 32 i + 16))
//   S5  c = r0 (2^32 - p)                      S6  r = r0 + (c >> 32), about 2^32 / m
//   S7  U r, V r                               S8  u = U r >> (24 + e), v the same, clamped to the texture
//   C1  texel address, cache read              C2  tag compare; a miss holds the pipeline
//   out byte write into the framebuffer, unless the texel is 0 and the texture see-through
//
// Texture cache: 16 KB direct mapped, lines of 32 bytes (index = address[13:5], tag =
// address[25:14]), filled with one line read from the SDRAM. On a miss the pipeline stops,
// the line comes in (8 words), a clock for the last word and the tag to be written, the
// missed pixel's address is read again (one clock) and the pipeline goes on. Invalidated
// after a reset and on request (512 clocks).
//

module raster_tex (
    input   wire            clk,
    input   wire            reset,
    input   wire            invalidate,         // one clock: forget every cached line

    // pixels in: Q, U, V with 8 fraction bits, framebuffer word and pixel in it
    input   wire            in_valid,
    output  wire            in_ready,
    input   wire    [43:0]  in_q,
    input   wire    [43:0]  in_u,
    input   wire    [43:0]  in_v,
    input   wire    [13:0]  in_word,
    input   wire    [1:0]   in_lane,

    // the triangle's texture: SDRAM byte address of texel (0, 0), rows of 256 bytes
    input   wire    [25:0]  tex_base,
    input   wire    [8:0]   tex_w,              // 1..256
    input   wire    [8:0]   tex_h,
    input   wire            tex_cut,            // texel 0 is see-through

    output  wire            busy,
    output  reg     [31:0]  misses = 0,         // lines fetched since the last clear
    output  wire            stalled,            // the pipeline waits for a line (or the invalidation)
    input   wire            clear_misses,

    // framebuffer writes, one pixel each
    output  reg             out_wr = 0,
    output  reg     [13:0]  out_word,
    output  reg     [3:0]   out_be,
    output  reg     [7:0]   out_texel,

    // line reads from the SDRAM (sdram_port)
    output  reg             mem_valid = 0,
    input   wire            mem_ready,
    output  reg     [25:5]  mem_addr,
    input   wire            mem_rsp_valid,
    input   wire    [31:0]  mem_rsp_data
);

    localparam  [2:0]   M_INVAL  = 3'd0;        // clearing the tags
    localparam  [2:0]   M_NORMAL = 3'd1;
    localparam  [2:0]   M_REQ    = 3'd2;        // line read asked for
    localparam  [2:0]   M_DATA   = 3'd3;        // its words coming in
    localparam  [2:0]   M_REPLAY = 3'd4;        // read the missed address again
    localparam  [2:0]   M_SETTLE = 3'd5;        // the last word and the tag are written

    reg  [2:0]  mstate = M_INVAL;
    reg  [8:0]  inval_cnt = 0;
    reg  [2:0]  fill_cnt;

    wire        stall;
    wire        adv = !stall;
    assign in_ready = adv;

//
// reciprocal table: r0[i] = floor((2^32 - 1) / (2^15 + 32 i + 16))
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

//
// pipeline registers
//
    reg         v1 = 0, v2 = 0, v3 = 0, v4 = 0, v5 = 0, v6 = 0, v7 = 0, v8 = 0, c1 = 0, c2 = 0;
    reg  [15:0] w1, w2, w3, w4, w5, w6, w7, w8, wc1, wc2;  // {framebuffer word, lane}
    reg signed [43:0] q1, u1, va1;
    reg  [30:0] q2;
    reg signed [26:0] u2, u3, u4, u5, u6, u7;
    reg signed [26:0] vv2, vv3, vv4, vv5, vv6, vv7;
    reg  [15:0] m3, m4;
    reg signed [5:0] e3, e4, e5, e6, e7, e8;
    reg  [16:0] r0_4, r0_5, r0_6;
    reg  [32:0] p5;
    reg signed [41:0] c6;
    reg  [17:0] r7;
    reg signed [45:0] up8, vp8;
    reg  [7:0]  tu9, tv9;
    reg         v9 = 0;
    reg  [15:0] w9;
    reg  [25:0] a_c1, a_c2;                     // texel byte address in the SDRAM

    // S1 -> S2
    wire signed [35:0] q1s = q1[43:8];             // >> 8
    wire signed [35:0] u1s = u1[43:8];
    wire signed [35:0] v1s = va1[43:8];
    wire [30:0] q_lim = q1s < 36'sd1 ? 31'd1 : q1s > 36'sh7FFFFFFF ? 31'h7FFFFFFF : q1s[30:0];
    wire signed [26:0] u_lim = u1s < -36'sd67108864 ? -27'sd67108864 : u1s > 36'sd67108863 ? 27'sd67108863 : u1s[26:0];
    wire signed [26:0] v_lim = v1s < -36'sd67108864 ? -27'sd67108864 : v1s > 36'sd67108863 ? 27'sd67108863 : v1s[26:0];

    // S2 -> S3: bit length of q2, e = length - 16, m = q2 shifted to 16 bits
    reg  [4:0]  q_len;
    integer     bi;
always @(*) begin
    q_len = 0;
    for(bi = 0; bi < 31; bi = bi + 1) if(q2[bi]) q_len = bi[4:0] + 5'd1;
end
    wire signed [5:0] q_e = $signed({1'b0, q_len}) - 6'sd16;
    wire [46:0] q_wide = {q2, 16'd0};                           // q2 << 16
    wire [15:0] q_m = q_wide[{1'b0, q_len} +: 16];             // bits [len-1 .. len-16] of q2

    // S5 -> S6: c = r0 (2^32 - p), 2^32 - p is small
    wire signed [33:0] t5 = $signed({1'b0, 33'h1_0000_0000}) - $signed({1'b0, p5});
    wire signed [23:0] t5s = t5[23:0];

    // S8 -> S9: u = U r >> (24 + e), clamped
    wire [5:0]  sh8 = 6'd24 + e8[5:0];
    wire signed [45:0] tu_raw = up8 >>> sh8;
    wire signed [45:0] tv_raw = vp8 >>> sh8;
    wire [7:0]  tu_lim = tu_raw < 46'sd0 ? 8'd0 : tu_raw >= $signed({37'd0, tex_w}) ? tex_w[7:0] - 8'd1 : tu_raw[7:0];
    wire [7:0]  tv_lim = tv_raw < 46'sd0 ? 8'd0 : tv_raw >= $signed({37'd0, tex_h}) ? tex_h[7:0] - 8'd1 : tv_raw[7:0];

//
// cache memories
//
    reg  [31:0] dmem [0:4095];
    reg  [12:0] tmem [0:511];                   // {valid, tag}
    reg  [31:0] d_q;
    reg  [12:0] t_q;
    wire [25:0] rd_addr = mstate == M_REPLAY ? a_c2 : a_c1;
    reg         d_we, t_we;
    reg  [11:0] d_wa;
    reg  [8:0]  t_wa;
    reg  [31:0] d_wd;
    reg  [12:0] t_wd;

always @(posedge clk) begin
    d_q <= dmem[rd_addr[13:2]];
    t_q <= tmem[rd_addr[13:5]];
    if(d_we) dmem[d_wa] <= d_wd;
    if(t_we) tmem[t_wa] <= t_wd;
end

    wire        hit = t_q[12] && t_q[11:0] == a_c2[25:14];
    wire [7:0]  texel = d_q[8 * a_c2[1:0] +: 8];
    assign stall = mstate != M_NORMAL || (c2 && !hit);
    assign stalled = stall;
    assign busy = mstate != M_NORMAL || v1 || v2 || v3 || v4 || v5 || v6 || v7 || v8 || v9 ||
                  c1 || c2 || out_wr;

always @(posedge clk) begin
    out_wr <= 0;
    d_we <= 0;
    t_we <= 0;

    if(adv) begin
        // S1
        v1 <= in_valid;
        q1 <= in_q; u1 <= in_u; va1 <= in_v;
        w1 <= {in_word, in_lane};
        // S2
        v2 <= v1; w2 <= w1;
        q2 <= q_lim; u2 <= u_lim; vv2 <= v_lim;
        // S3
        v3 <= v2; w3 <= w2;
        m3 <= q_m; e3 <= q_e; u3 <= u2; vv3 <= vv2;
        // S4: table read
        v4 <= v3; w4 <= w3;
        r0_4 <= rtab[m3[14:5]];
        m4 <= m3; e4 <= e3; u4 <= u3; vv4 <= vv3;
        // S5: p = m r0
        v5 <= v4; w5 <= w4;
        p5 <= m4 * r0_4;
        r0_5 <= r0_4; e5 <= e4; u5 <= u4; vv5 <= vv4;
        // S6: c = r0 (2^32 - p)
        v6 <= v5; w6 <= w5;
        c6 <= $signed({1'b0, r0_5}) * t5s;
        r0_6 <= r0_5; e6 <= e5; u6 <= u5; vv6 <= vv5;
        // S7: r = r0 + (c >> 32)
        v7 <= v6; w7 <= w6;
        r7 <= {1'b0, r0_6} + {{8{c6[41]}}, c6[41:32]};     // c >> 32, signed
        e7 <= e6; u7 <= u6; vv7 <= vv6;
        // S8: U r, V r
        v8 <= v7; w8 <= w7;
        up8 <= u7 * $signed({1'b0, r7});
        vp8 <= vv7 * $signed({1'b0, r7});
        e8 <= e7;
        // S9: texel coordinates
        v9 <= v8; w9 <= w8;
        tu9 <= tu_lim; tv9 <= tv_lim;
        // C1: texel address, cache read
        c1 <= v9; wc1 <= w9;
        a_c1 <= tex_base + {10'd0, tv9, tu9};
        // C2: compare
        c2 <= c1; wc2 <= wc1; a_c2 <= a_c1;
        // out
        if(c2) begin
            out_wr <= !(tex_cut && texel == 8'd0);
            out_word <= wc2[15:2];
            out_be <= 4'b1000 >> wc2[1:0];
            out_texel <= texel;
        end
    end

    // misses, fills, invalidation
    case(mstate)
    M_INVAL: begin
        t_we <= 1;
        t_wa <= inval_cnt;
        t_wd <= 13'd0;
        inval_cnt <= inval_cnt + 1'b1;
        if(inval_cnt == 9'd511) mstate <= M_NORMAL;
    end
    M_NORMAL: if(c2 && !hit) begin
        misses <= misses + 1'b1;
        mem_valid <= 1;
        mem_addr <= a_c2[25:5];
        mstate <= M_REQ;
    end
    M_REQ: if(mem_ready) begin
        mem_valid <= 0;
        fill_cnt <= 0;
        mstate <= M_DATA;
    end
    M_DATA: if(mem_rsp_valid) begin
        d_we <= 1;
        d_wa <= {mem_addr[13:5], fill_cnt};
        d_wd <= mem_rsp_data;
        fill_cnt <= fill_cnt + 1'b1;
        if(fill_cnt == 3'd7) begin
            t_we <= 1;
            t_wa <= mem_addr[13:5];
            t_wd <= {1'b1, mem_addr[25:14]};
            mstate <= M_SETTLE;
        end
    end
    M_SETTLE: mstate <= M_REPLAY;           // a read now would still see the old line
    default: mstate <= M_NORMAL;            // M_REPLAY: one clock
    endcase

    if(clear_misses) misses <= 0;
    if(reset || invalidate) begin
        mstate <= M_INVAL;
        inval_cnt <= 0;
        mem_valid <= 0;
    end
    if(reset) begin
        v1 <= 0; v2 <= 0; v3 <= 0; v4 <= 0; v5 <= 0; v6 <= 0; v7 <= 0; v8 <= 0; v9 <= 0;
        c1 <= 0; c2 <= 0;
    end
end

endmodule
