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
//
    reg         run = 0, pend = 0;
    reg  [1:0]  drain = 0;                      // clocks left until the sums are complete
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

    // fetching a line for voice f_v
    reg         f_busy = 0;
    reg  [4:0]  f_v;
    reg  [2:0]  f_w;
    wire        adv = run && ph == 2'd3 && !(need && f_busy);

    // the multiplier and the sums: operands, product, sum
    reg  [7:0]  m_a, m_b;
    reg         m_en = 0, m_side, r_load = 0;
    reg  [7:0]  r_smp;
    reg         r_en;
    reg signed [16:0] prod;
    reg         p_en = 0, p_side;
    reg signed [20:0] acc_l, acc_r;
    wire signed [20:0] acc_sum = (p_side ? acc_r : acc_l) + {{4{prod[16]}}, prod};

    function [15:0] clip(input signed [20:0] a);
        clip = a > 21'sd32767 ? 16'h7FFF : a < -21'sd32768 ? 16'h8000 : a[15:0];
    endfunction

    assign busy = run || pend || drain != 2'd0;

always @(posedge clk) begin
    q_addr <= r_addr[v];
    q_end <= r_end[v];
    q_loop <= r_loop[v];
    q_step <= r_step[v];
    q_vol <= r_vol[v];
    q_pos <= r_pos[v];
    lq <= lines[{v, cur[4:2]}];
    if(adv && on && !stop) r_pos[v] <= npos;
    if(f_busy && !mem_valid && mem_rsp_valid) lines[{f_v, f_w}] <= mem_rsp_data;
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
            f_w <= 0;
            f_busy <= 1;
        end
        v <= v + 1'b1;
        if(v == 5'd31) begin
            run <= 0;
            drain <= 2'd3;
        end
    end

    // lines in
    if(mem_valid && mem_ready) mem_valid <= 0;
    if(f_busy && !mem_valid && mem_rsp_valid) begin
        f_w <= f_w + 1'b1;
        if(f_w == 3'd7) f_busy <= 0;
    end

    // the multiplier: left half in phase 3, the right half a clock later
    m_en <= 0;
    r_load <= adv;
    if(adv) begin
        m_a <= smp;
        m_b <= q_vol[7:0];
        m_en <= on && !new_;
        m_side <= 0;
        r_smp <= smp;
        r_en <= on && !new_;
    end
    if(r_load) begin
        m_a <= r_smp;
        m_b <= q_vol[15:8];                     // (still the voice before's: read a clock late)
        m_en <= r_en;
        m_side <= 1;
    end
    prod <= $signed({{9{m_a[7]}}, m_a}) * $signed({9'd0, m_b});
    p_en <= m_en;
    p_side <= m_side;
    if(p_en) begin
        if(p_side) acc_r <= acc_sum;
        else acc_l <= acc_sum;
    end
    if(drain != 2'd0) begin
        drain <= drain - 1'b1;
        if(drain == 2'd1) begin
            out_l <= clip(p_en && !p_side ? acc_sum : acc_l);
            out_r <= clip(p_en && p_side ? acc_sum : acc_r);
        end
    end

    // a pass begins with the frame, once the line before is in
    if(!run && drain == 2'd0 && pend && !f_busy) begin
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

    if(reset) begin
        playing <= 0;
        fresh <= 0;
        run <= 0;
        pend <= 0;
        drain <= 0;
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
