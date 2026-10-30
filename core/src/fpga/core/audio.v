//
// Audio: the sound effects mixer and its I2S output, bit for bit the C model firmware/mixer.c.
// 32 voices play 8-bit unsigned samples from the SDRAM. Once per output frame (48 kHz) one pass
// goes through them, a voice every four clocks, all through one datapath: the byte at the
// voice's place (the stored sample, nothing smoothed between two, as the game's driver played
// them), centred, times the left and the right volume, summed and clipped to 16 bits; then the
// place moves on by the voice's step, round its loop or to its end, where the voice stops.
// A voice's line of 32 bytes around its place lies in block RAM; when the place moves into
// another line, the pass asks for that line over the shared line port (soc.v), and it is in
// long before the next pass reads it (a frame is 1547 clocks).
// After the voices the pass plays the music: one stream of 16-bit stereo samples at 44.1 kHz
// (the CD's) from a ring of 256 KB in the SDRAM that the firmware keeps filled. The two samples
// around the music's place, each side between them in a straight line (16 bits of fraction),
// times the volume, go into the sums; then the place moves on by 44100 / 48000 of a sample
// (60211 / 65536). The two lines around the place (16 samples) lie in an MLAB; when the place
// moves into the second, the pass asks for the one after it.
//
// Registers (soc.v maps them at 0xF000_4000 + wr_addr):
//   32 v + 0   voice v: [25:0] SDRAM byte address of the sample's first byte
//   32 v + 4   [25:0] the byte address where it ends, or where its loop ends
//   32 v + 8   [19:0] the loop's length in bytes: at the end the place goes back by it;
//              [31:24] how often: 0 never (the voice stops at the end), 255 for ever
//   32 v + 12  [17:0] the step per output frame in samples, 2.16 (the game's pitch, 16.16 of
//              the sample's own rate, times 11025 / 48000)
//   32 v + 16  [7:0] left, [15:8] right: the centred byte times these
//   0x400      write: bit v (re)starts voice v at its first byte, with its loop count; the
//              pass after next plays that byte (the next one sets the place and loads its
//              line); read: bit v voice v plays
//   0x404      write: bit v stops voice v
//   0x408      read: bit 0 a pass was not done by the next frame since the last clear (that
//              frame repeats the one before); write: clear
//   0x40C      the music: [25:18] the SDRAM byte address of its ring (256 KB; a frame a word,
//              the left sample in the low half, as the CD has them); set before a start
//   0x410      [15:0] the music's volume: the sample times this, >> 15
//   0x414      write: bit 0 1 plays the ring from its start (the two passes after load its
//              lines, the third plays the first sample), 0 stops; read: [15:0] the place, the
//              sample in the ring the next pass starts from, bit 16 it plays
// A register takes effect when the pass comes to its voice: writes between passes all at once.
//
// I2S as the APF takes it (the template's timing): MCLK 12.288 MHz from a fractional divider,
// the bit clock MCLK / 4 (not a pin), 64 bits a frame; LRCK low the left half, high the right,
// each 16 bits MSB first from the second bit clock of the half on, then zeros. Everything
// changes in the clock in which the bit clock falls, which is a rising edge of MCLK.
//

module audio (
    input   wire            clk,
    input   wire            reset,              // the CPU's: the voices stop, the output goes on

    input   wire            wr,
    input   wire    [10:2]  wr_addr,
    input   wire    [31:0]  wr_data,
    output  reg     [31:0]  playing = 0,
    output  reg             late = 0,
    output  wire            busy,               // a pass runs or waits (the testbench writes between)
    output  wire    [16:0]  music,              // {it plays, the place in the ring}

    // lines from the SDRAM (soc.v's line port)
    output  reg             mem_valid = 0,
    input   wire            mem_ready,
    output  reg     [25:5]  mem_addr,
    input   wire            mem_rsp_valid,
    input   wire    [31:0]  mem_rsp_data,

    output  reg             i2s_mclk = 0,
    output  reg             i2s_sclk = 0,       // the bit clock: no pin, the simulation reads it
    output  reg             i2s_lrck = 0,
    output  reg             i2s_dac = 0
);

//
// I2S: 74.25 MHz x 4096 / 12375 = 24.576 MHz, the edges of MCLK
//
    reg  [13:0] ac = 0;
    wire [14:0] ac_n = {1'b0, ac} + 15'd4096;
    wire        m_edge = ac_n >= 15'd12375;
    reg  [2:0]  mc = 0;                         // MCLK edges: MCLK ~mc[0], the bit clock mc[2]
    wire [2:0]  mc_n = mc + 1'b1;
    reg  [5:0]  bitn = 0;                       // the bit clock of the frame, 0: LRCK fell
    wire [5:0]  bitn_n = bitn + 1'b1;
    wire        s_fall = m_edge && mc == 3'd7;
    wire        tick = s_fall && bitn_n == 6'd0;    // a frame begins
    reg  [15:0] sh = 0;                         // the half being sent
    reg  [15:0] out_l = 0, out_r = 0;           // the last pass's sums
    reg  [15:0] r_next = 0;                     // its right half, for the frame being sent
always @(posedge clk) begin
    ac <= m_edge ? ac_n[13:0] - 14'd12375 : ac_n[13:0];
    if(m_edge) begin
        mc <= mc_n;
        i2s_mclk <= ~mc_n[0];
        i2s_sclk <= mc_n[2];
    end
    if(s_fall) begin
        bitn <= bitn_n;
        i2s_lrck <= bitn_n[5];
        i2s_dac <= sh[15];
        sh <= bitn_n[4:0] != 5'd0 ? {sh[14:0], 1'b0} : bitn_n[5] ? r_next : out_l;
        if(tick) r_next <= out_r;
    end
end

//
// the registers, one small RAM each (MLAB): the CPU's port writes, the pass reads at v
//
    (* ramstyle = "MLAB, no_rw_check" *) reg  [25:0] r_addr [0:31];
    (* ramstyle = "MLAB, no_rw_check" *) reg  [25:0] r_end [0:31];
    (* ramstyle = "MLAB, no_rw_check" *) reg  [27:0] r_loop [0:31];     // {times, length}
    (* ramstyle = "MLAB, no_rw_check" *) reg  [17:0] r_step [0:31];
    (* ramstyle = "MLAB, no_rw_check" *) reg  [15:0] r_vol [0:31];
    // the place: {byte address, fraction, loops left}, the pass's own
    (* ramstyle = "MLAB, no_rw_check" *) reg  [49:0] r_pos [0:31];
    // the lines: 8 words a voice
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] lines [0:255];
    // the music's two lines: the sample at place p in word p mod 16
    (* ramstyle = "MLAB, no_rw_check" *) reg  [31:0] mlines [0:15];

    wire        wr_voice = wr && !wr_addr[10];
    wire [4:0]  wv = wr_addr[9:5];
always @(posedge clk) begin
    if(wr_voice && wr_addr[4:2] == 3'd0) r_addr[wv] <= wr_data[25:0];
    if(wr_voice && wr_addr[4:2] == 3'd1) r_end[wv] <= wr_data[25:0];
    if(wr_voice && wr_addr[4:2] == 3'd2) r_loop[wv] <= {wr_data[31:24], wr_data[19:0]};
    if(wr_voice && wr_addr[4:2] == 3'd3) r_step[wv] <= wr_data[17:0];
    if(wr_voice && wr_addr[4:2] == 3'd4) r_vol[wv] <= wr_data[15:0];
end

//
// the pass: a voice in four phases, read address v
//   0  the RAMs read voice v (and the voice before's right half goes to the multiplier)
//   1  the place plus the step; its line's word asked for
//   2  past the end?, the place back by the loop; the byte
//   3  the new place written back, its line asked for if it is another one (the pass waits
//      while the line before is still coming), the left half to the multiplier
// then the music in eight clocks, mph:
//   0, 1  the samples at the place and after it read
//   2, 3  left, right: the second minus the first to the multiplier, times the fraction
//   4, 5  left, right: the first plus that >> 16 to the multiplier, times the volume
//   6, 7  into the sums (>> 15); in 7 the frame is complete, the place moves on, and when it
//         came into the second line, the line after is asked for (waits while one is coming)
//
    reg         run = 0, pend = 0;
    reg         mrun = 0;
    reg  [2:0]  mph = 0;
    reg  [4:0]  v = 0;
    reg  [1:0]  ph = 0;
    reg  [31:0] fresh = 0;                      // (re)started: the place is set by the next pass
    reg  [25:0] q_addr, q_end;
    reg  [27:0] q_loop;
    reg  [17:0] q_step;
    reg  [15:0] q_vol;
    reg  [49:0] q_pos;
    reg  [31:0] lq;
    wire [25:0] cur = q_pos[49:24];
    wire [7:0]  left = q_pos[7:0];              // loops left
    reg  [41:0] sum;                            // {byte address, fraction}
    reg  [25:0] back;                           // sum's address back by the loop
    reg         over, on, new_;
    reg  [1:0]  bsel;
    reg  [7:0]  smp;                            // the byte, centred (two's complement)

    // phase 3
    wire        stop = !new_ && over && left == 8'd0;
    wire [49:0] npos = new_ ? {q_addr, 16'd0, q_loop[27:20]} :
                       over ? {back, sum[15:0], left == 8'hFF ? left : left - 8'd1} :
                              {sum, left};
    wire        need = on && !stop && (new_ || npos[49:29] != cur[25:5]);

    // fetching a line for voice f_v, or the music's (f_mus) into half f_v[0]
    reg         f_busy = 0, f_mus = 0;
    reg  [4:0]  f_v;
    reg  [2:0]  f_w;
    wire        adv = run && ph == 2'd3 && !(need && f_busy);

    // the music: its registers and place
    reg  [25:18] mu_base = 0;
    reg  [15:0] mu_vol = 0;
    reg         mu_on = 0;
    reg  [1:0]  mu_fill = 0;                    // lines to load before it plays
    reg  [15:0] mu_idx = 0, mu_frac = 0;        // the place: the sample in the ring, the fraction
    reg  [31:0] mq, ms0;                        // the word read, the first sample's
    wire        mplay = mu_on && mu_fill == 2'd0;
    wire [3:0]  ma = mu_idx[3:0] + {3'd0, mph != 3'd0};
    wire [16:0] mdiff = mph[0] ? {mq[31], mq[31:16]} - {ms0[31], ms0[31:16]}
                               : {mq[15], mq[15:0]} - {ms0[15], ms0[15:0]};
    wire [16:0] mf_sum = {1'b0, mu_frac} + 17'd60211;
    wire [15:0] mi_next = mu_idx + {15'd0, mf_sum[16]};
    // the line to load: from the start the place's and the one after, while playing the line
    // after the one the place came into
    wire [12:0] mline = mu_fill != 2'd0 ? mu_idx[15:3] + {12'd0, mu_fill[0]} : mi_next[15:3] + 13'd1;
    wire        mneed = mu_on && (mu_fill != 2'd0 || (mf_sum[16] && mi_next[2:0] == 3'd0));
    wire        madv = mrun && mph == 3'd7 && !(mneed && f_busy);
    assign music = {mu_on, mu_idx};

    // the multiplier and the sums: operands, product, sum (a music product counts >> 15)
    reg  [17:0] m_a;
    reg  [15:0] m_b;
    reg         m_en = 0, m_side, m_mus, r_load = 0;
    reg  [7:0]  r_smp;
    reg         r_en;
    reg signed [35:0] prod;
    reg         p_en = 0, p_side, p_mus;
    reg signed [21:0] acc_l, acc_r;
    wire signed [21:0] acc_sum = (p_side ? acc_r : acc_l) +
                                 (p_mus ? {{5{prod[31]}}, prod[31:15]} : {{5{prod[16]}}, prod[16:0]});
    // the music's first sample plus the difference times the fraction >> 16
    wire [17:0] m_int = (mph[0] ? {{2{ms0[31]}}, ms0[31:16]} : {{2{ms0[15]}}, ms0[15:0]}) +
                        {prod[32], prod[32:16]};

    function [15:0] clip(input signed [21:0] a);
        clip = a > 22'sd32767 ? 16'h7FFF : a < -22'sd32768 ? 16'h8000 : a[15:0];
    endfunction

    assign busy = run || mrun || pend;

always @(posedge clk) begin
    q_addr <= r_addr[v];
    q_end <= r_end[v];
    q_loop <= r_loop[v];
    q_step <= r_step[v];
    q_vol <= r_vol[v];
    q_pos <= r_pos[v];
    lq <= lines[{v, cur[4:2]}];
    mq <= mlines[ma];
    if(adv && on && !stop) r_pos[v] <= npos;
    if(f_busy && !mem_valid && mem_rsp_valid && !f_mus) lines[{f_v, f_w}] <= mem_rsp_data;
    if(f_busy && !mem_valid && mem_rsp_valid && f_mus) mlines[{f_v[0], f_w}] <= mem_rsp_data;
end

    integer     i;
always @(posedge clk) begin
    if(tick) pend <= 1;
    if(tick && busy) late <= 1;
    if(run) ph <= ph + 1'b1;

    if(run && ph == 2'd1) begin
        sum <= {cur, q_pos[23:8]} + {24'd0, q_step};
        on <= playing[v];
        new_ <= fresh[v];
        bsel <= cur[1:0];
    end
    if(run && ph == 2'd2) begin
        over <= sum[41:16] >= q_end;
        back <= sum[41:16] - {6'd0, q_loop[19:0]};
        smp <= lq[{bsel, 3'd0} +: 8] ^ 8'h80;
    end
    if(run && ph == 2'd3 && !adv) ph <= ph;     // waits for the line before
    if(adv) begin
        if(stop && !fresh[v]) playing[v] <= 0;  // (unless started again meanwhile)
        if(new_) fresh[v] <= 0;
        if(need) begin
            mem_valid <= 1;
            mem_addr <= npos[49:29];
            f_v <= v;
            f_mus <= 0;
            f_w <= 0;
            f_busy <= 1;
        end
        v <= v + 1'b1;
        if(v == 5'd31) begin
            run <= 0;
            mrun <= 1;
            mph <= 0;
        end
    end

    // the music after the voices
    if(mrun && mph != 3'd7) mph <= mph + 1'b1;
    if(mrun && mph == 3'd1) ms0 <= mq;
    if(madv) begin
        mrun <= 0;
        if(mplay) begin
            mu_frac <= mf_sum[15:0];
            mu_idx <= mi_next;
        end
        if(mneed) begin
            mem_valid <= 1;
            mem_addr <= {mu_base, mline};
            f_v <= {4'd0, mline[0]};
            f_mus <= 1;
            f_w <= 0;
            f_busy <= 1;
            if(mu_fill != 2'd0) mu_fill <= mu_fill - 1'b1;
        end
    end

    // lines in
    if(mem_valid && mem_ready) mem_valid <= 0;
    if(f_busy && !mem_valid && mem_rsp_valid) begin
        f_w <= f_w + 1'b1;
        if(f_w == 3'd7) f_busy <= 0;
    end

    // the multiplier: left half in phase 3, the right half a clock later; the music's
    // differences times the fraction in mph 2 and 3, its samples times the volume in 4 and 5
    m_en <= 0;
    r_load <= adv;
    if(adv) begin
        m_a <= {{10{smp[7]}}, smp};
        m_b <= {8'd0, q_vol[7:0]};
        m_en <= on && !new_;
        m_side <= 0;
        m_mus <= 0;
        r_smp <= smp;
        r_en <= on && !new_;
    end
    if(r_load) begin
        m_a <= {{10{r_smp[7]}}, r_smp};
        m_b <= {8'd0, q_vol[15:8]};             // (still the voice before's: read a clock late)
        m_en <= r_en;
        m_side <= 1;
        m_mus <= 0;
    end
    if(mrun && (mph == 3'd2 || mph == 3'd3)) begin
        m_a <= {mdiff[16], mdiff};
        m_b <= mu_frac;
    end
    if(mrun && (mph == 3'd4 || mph == 3'd5)) begin
        m_a <= m_int;
        m_b <= mu_vol;
        m_en <= mplay;
        m_side <= mph[0];
        m_mus <= 1;
    end
    prod <= $signed(m_a) * $signed({2'b00, m_b});
    p_en <= m_en;
    p_side <= m_side;
    p_mus <= m_mus;
    if(p_en) begin
        if(p_side) acc_r <= acc_sum;
        else acc_l <= acc_sum;
    end
    if(madv) begin
        out_l <= clip(p_en && !p_side ? acc_sum : acc_l);
        out_r <= clip(p_en && p_side ? acc_sum : acc_r);
    end

    // a pass begins with the frame, once the line before is in
    if(!run && !mrun && pend && !f_busy) begin
        pend <= 0;
        run <= 1;
        v <= 0;
        ph <= 0;
        acc_l <= 0;
        acc_r <= 0;
    end

    // the CPU starts and stops voices, bit by bit over the pass's own changes
    for(i = 0; i < 32; i = i + 1) begin
        if(wr && wr_addr[10:2] == 9'h100 && wr_data[i]) begin
            playing[i] <= 1;
            fresh[i] <= 1;
        end
        if(wr && wr_addr[10:2] == 9'h101 && wr_data[i]) playing[i] <= 0;
    end
    if(wr && wr_addr[10:2] == 9'h102) late <= 0;
    if(wr && wr_addr[10:2] == 9'h103) mu_base <= wr_data[25:18];
    if(wr && wr_addr[10:2] == 9'h104) mu_vol <= wr_data[15:0];
    if(wr && wr_addr[10:2] == 9'h105) begin
        mu_on <= wr_data[0];
        mu_fill <= 2'd2;
        mu_idx <= 0;
        mu_frac <= 0;
    end

    if(reset) begin
        playing <= 0;
        fresh <= 0;
        run <= 0;
        mrun <= 0;
        mu_on <= 0;
        pend <= 0;
        mem_valid <= 0;
        f_busy <= 0;
        m_en <= 0;
        p_en <= 0;
        r_load <= 0;
        late <= 0;
        out_l <= 0;
        out_r <= 0;
    end
end

endmodule
