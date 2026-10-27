//
// The rasterizer's texture path: one pixel a clock through a pipeline, the same result as
// the C model firmware/raster.c (span_texfx). Per pixel, from acc = Q, U, V with 8
// fraction bits:
//   S1  Q = acc_Q >> 8 limited to [1, 2^31 - 1]; U, V = acc >> 8 limited to 27 bits signed
//   S2  e = bit length of Q - 16, m = Q shifted to 16 bits
//   S3  table address m[14:5]                  S4  p = m r0, r0 = floor((2^32-1) / (2^15 + 32 i + 16))
//   S5  c = r0 (2^32 - p)                      S6  r = r0 + (c >> 32), about 2^32 / m
//   S7  U r, V r                               S8  u = U r >> (24 + e), v the same, clamped to the texture
//   A   texel address, tag read                T   tag compare, into the queue
// then a queue of 64 pixels and the back end: texel out of the cache, byte into the
// framebuffer unless the texel is 0 and the texture see-through.
//
// Texture cache with prefetch (Igehy, Eldridge, Proudfoot: "Prefetching in a Texture
// Cache Architecture", 1998): 16 KB direct mapped, lines of 32 bytes (index =
// address[13:5], tag = address[25:14]). The tags are checked at T, ahead of the queue: a
// miss writes the new tag at once and asks for the line (up to 16 lines under way, read
// from the SDRAM one after the other into a line buffer); the pixel goes into the queue
// marked; two lines may be asked for at once (the line port starts the second while the
// words of the first come in). The back end takes the pixels in order: a marked one waits
// until its line is in, writes it into the cache and takes its texel from the line buffer. So a line is
// replaced only after every pixel before it has used the old one, and the loading runs
// while the pixels before it are drawn. Invalidated after a reset and on request (512
// clocks).
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

    // the triangle's texture: SDRAM byte address of texel (0, 0), rows of 256 bytes; used
    // up to T, so they may change once front_busy is low
    input   wire    [25:0]  tex_base,
    input   wire    [8:0]   tex_w,              // 1..256
    input   wire    [8:0]   tex_h,
    input   wire            tex_cut,            // texel 0 is see-through

    output  wire            busy,
    output  wire            front_busy,         // pixels before the queue
    output  reg     [31:0]  misses = 0,         // lines fetched since the last clear
    output  wire            stalled,            // the back end waits for a line
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
// front: pipeline registers
//
    reg         v1 = 0, v2 = 0, v3 = 0, v4 = 0, v5 = 0, v6 = 0, v7 = 0, v8 = 0, v9 = 0, va = 0, vt = 0;
    reg  [15:0] w1, w2, w3, w4, w5, w6, w7, w8, w9, wa, wt;    // {framebuffer word, lane}
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
    reg  [25:0] a_a, a_t;                       // texel byte address in the SDRAM

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

    assign front_busy = v1 || v2 || v3 || v4 || v5 || v6 || v7 || v8 || v9 || va || vt;

//
// tags: read when the pixel moves from A to T (held while T waits), written at T on a
// miss; the pixel right behind gets that write forwarded (the RAM would give the old tag)
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [12:0] tags [0:511];    // {valid, tag}
    reg  [12:0] t_q;
    reg         inval = 1;
    reg  [8:0]  inval_cnt = 0;
    reg         t_fwd = 0;
    reg  [11:0] t_fwd_tag;
    reg         t_stale = 0;                    // T's tag was read before an invalidation
    wire [12:0] tag_t = t_fwd ? {1'b1, t_fwd_tag} : t_q;
    wire        hit_t = !t_stale && tag_t[12] && tag_t[11:0] == a_t[25:14];

//
// line slots: the lines asked for, in order, each a line address and its 8 words (one
// small RAM per word, so a whole line reads in one clock)
//
    reg  [20:0] s_addr [0:15];
    reg  [15:0] s_ready = 0;
    reg  [4:0]  s_alloc = 0, s_req = 0, s_fetch = 0, s_use = 0;    // a wrap bit over the slot number
    wire [4:0]  s_used = s_alloc - s_use;
    wire        s_free = s_used != 5'd16;
    wire [3:0]  s_ru = s_use[3:0];
    reg  [2:0]  fill_cnt = 0;
    reg         f_last = 0;                     // a line's last word came in the clock before
    reg  [3:0]  f_slot;
    wire        f_we = mem_rsp_valid && s_fetch != s_req;      // words of the oldest line asked for
    wire [7:0]  f_lane = 8'd1 << fill_cnt;

    genvar      gi;
generate for(gi = 0; gi < 8; gi = gi + 1) begin : lbuf
    (* ramstyle = "MLAB, no_rw_check" *) reg  [31:0] words [0:15];
    reg  [31:0] q;
    always @(posedge clk) begin
        if(f_we && f_lane[gi]) words[s_fetch[3:0]] <= mem_rsp_data;
        q <= words[s_ru];
    end
end endgenerate
    wire [255:0] line_q = {lbuf[7].q, lbuf[6].q, lbuf[5].q, lbuf[4].q,
                           lbuf[3].q, lbuf[2].q, lbuf[1].q, lbuf[0].q};
    reg         lq_ok = 0;                      // line_q holds the slot s_use

//
// the queue: 64 x {marked, index, offset in the line, framebuffer word and pixel,
// see-through}; its head read a clock ahead (a pixel written into the empty queue waits
// one clock more)
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] pq [0:63];
    reg  [31:0] pq_q;
    reg         q_ok = 0;                       // pq_q holds the entry at pq_rp
    reg  [6:0]  pq_wp = 0, pq_rp = 0;
    wire [6:0]  pq_n = pq_wp - pq_rp;
    wire        h_fill = pq_q[31];
    wire [8:0]  h_index = pq_q[30:22];
    wire [4:0]  h_off = pq_q[21:17];
    wire [15:0] h_w = pq_q[16:1];
    wire        h_cut = pq_q[0];

    // T goes into the queue when there is room and, for a miss, a free slot
    wire        t_commit = vt && !inval && pq_n != 7'd64 && (hit_t || s_free);
    wire        t_miss = t_commit && !hit_t;
    wire        adv = !inval && (!vt || t_commit);
    assign in_ready = adv;

    // the back end takes the head when it is not marked or its line is in
    wire        h_go = q_ok && pq_n != 7'd0 && (!h_fill || (s_ready[s_ru] && lq_ok));
    wire [5:0]  pq_ra = h_go ? pq_rp[5:0] + 6'd1 : pq_rp[5:0];
    assign stalled = pq_n != 7'd0 && h_fill && !s_ready[s_ru];

always @(posedge clk) begin
    if(inval) tags[inval_cnt] <= 13'd0;
    else if(t_miss) tags[a_t[13:5]] <= {1'b1, a_t[25:14]};
    if(adv) t_q <= tags[a_a[13:5]];
    if(t_commit) pq[pq_wp[5:0]] <= {t_miss, a_t[13:5], a_t[4:0], wt, tex_cut};
    pq_q <= pq[pq_ra];
end

//
// cache lines: 512 x 32 bytes; a marked pixel writes its line
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [255:0] lines [0:511];
    reg  [255:0] l_q;
always @(posedge clk) begin
    if(h_go && h_fill) lines[h_index] <= line_q;
    l_q <= lines[h_index];
end

    // B2: the byte, from the line buffer for a marked pixel, else from the cache
    reg         b2 = 0, b2_fill, b2_cut;
    reg  [4:0]  b2_off;
    reg  [15:0] b2_w;
    reg  [7:0]  b2_texel;
    wire [7:0]  texel = b2_fill ? b2_texel : l_q[8 * b2_off +: 8];

    assign busy = inval || front_busy || pq_n != 7'd0 || b2 || out_wr || s_used != 5'd0 || f_last;

always @(posedge clk) begin
    out_wr <= 0;

    // front
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
        // A: texel address
        va <= v9; wa <= w9;
        a_a <= tex_base + {10'd0, tv9, tu9};
        // T
        vt <= va; wt <= wa;
        a_t <= a_a;
        t_fwd <= t_miss && a_a[13:5] == a_t[13:5];
        t_fwd_tag <= a_t[25:14];
    end
    if(inval) t_stale <= 1;
    else if(adv) t_stale <= 0;

    // T: into the queue; a miss takes a slot
    if(t_commit) pq_wp <= pq_wp + 1'b1;
    if(t_miss) begin
        s_addr[s_alloc[3:0]] <= a_t[25:5];
        s_alloc <= s_alloc + 1'b1;
        misses <= misses + 1'b1;
    end
    q_ok <= !(t_commit && pq_wp[5:0] == pq_ra);

    // memory: the slots' lines asked for in order, up to two under way; a slot is ready a clock
    // after its last word, when line_q can show it
    f_last <= 0;
    if(f_last) s_ready[f_slot] <= 1;
    if(!mem_valid && s_req != s_alloc && s_req - s_fetch < 5'd2) begin
        mem_valid <= 1;
        mem_addr <= s_addr[s_req[3:0]];
    end
    if(mem_valid && mem_ready) begin
        mem_valid <= 0;
        s_req <= s_req + 1'b1;
    end
    if(f_we) begin
        fill_cnt <= fill_cnt + 1'b1;
        if(fill_cnt == 3'd7) begin
            f_last <= 1;
            f_slot <= s_fetch[3:0];
            s_fetch <= s_fetch + 1'b1;
        end
    end

    // back end, B1: the head out of the queue; a marked pixel frees its slot
    b2 <= h_go;
    lq_ok <= !(h_go && h_fill);
    if(h_go) begin
        pq_rp <= pq_rp + 1'b1;
        b2_fill <= h_fill;
        b2_off <= h_off;
        b2_w <= h_w;
        b2_cut <= h_cut;
        b2_texel <= line_q[8 * h_off +: 8];
        if(h_fill) begin
            s_ready[s_ru] <= 0;
            s_use <= s_use + 1'b1;
        end
    end
    // B2: the byte into the framebuffer
    if(b2) begin
        out_wr <= !(b2_cut && texel == 8'd0);
        out_word <= b2_w[15:2];
        out_be <= 4'b1000 >> b2_w[1:0];
        out_texel <= texel;
    end

    // invalidation: every tag cleared; lines under way still come in
    if(inval) begin
        inval_cnt <= inval_cnt + 1'b1;
        if(inval_cnt == 9'd511) inval <= 0;
    end

    if(clear_misses) misses <= 0;
    if(reset || invalidate) begin
        inval <= 1;
        inval_cnt <= 0;
    end
    if(reset) begin
        v1 <= 0; v2 <= 0; v3 <= 0; v4 <= 0; v5 <= 0; v6 <= 0; v7 <= 0; v8 <= 0; v9 <= 0;
        va <= 0; vt <= 0; b2 <= 0;
        pq_wp <= 0; pq_rp <= 0; q_ok <= 0;
        s_alloc <= 0; s_req <= 0; s_fetch <= 0; s_use <= 0; s_ready <= 0; lq_ok <= 0;
        mem_valid <= 0;
        fill_cnt <= 0;
        f_last <= 0;
    end
end

endmodule
