//
// FM music: the OPL2 (jtopl/, JTOPL by Jose Tejada) of the AdLib that the game's MIDI music was
// written for, a queue of time-stamped register writes in front of it, and the resampling of its
// output to the mixer's 48 kHz (audio.v adds the result to its sums).
//
// The chip's clock is 3.579545 MHz, a clock enable from clk (74.25 MHz) by a fractional
// accumulator, 715909 / 14850000 per clock (the same ratio as 3579545 / 74250000, exact in
// the long run). The chip makes one sample every 72 of its clocks, 49716 a second; `now` counts
// them, from power-up, wrapping at 16 bits (1.3 s).
//
// The firmware runs once a frame (20 ms) but the music's notes are due to 8 ms: it writes a
// frame or two ahead, and each write waits in the queue until its time. A write is one word:
//   [31:16] the time, in `now` units, at which the write is made (the low 16 bits of it)
//   [15:8]  the chip's register, [7:0] the value
// The queue holds 256 writes (one M10K) and releases the first when `now - time` has
// not gone negative (16 bits, so a time at most 0.65 s from `now`), writes of the same time one
// after the other. Between two writes the chip needs 128 of its clocks (36 us): it takes a
// register's value into its operator when that comes round, once in 72 clocks, and forgets it
// at the next write (the original's driver waited about as long). A write that is late is made
// at once, the later ones after it: the music slips by what the firmware was late, no more.
//
// run (FM_CTRL bit 0) 0 holds the chip in reset (silent, its registers cleared) and the queue
// empty; set it and the chip is ready after 128 of its clocks, and the writes of the next
// song can be queued. A CPU reset clears it.
//
// Resampling 49716 Hz to 48 kHz: on each output frame (`frame`, one clock) the value at a point
// 48 kHz after the one before is taken from the two newest samples of the chip by a
// straight line between them: the fraction is the clocks since the newer one, over the
// clocks between two (1493.5, approximated by 65536 / 44). Without it every 28th frame would
// skip a sample, which sounds like a buzz on high notes. The result times the gain (8 bits,
// 32 = as the chip makes it) is limited to 16 bits. One multiplier does both products, in
// turn. The frame's value is ready 8 clocks after `frame`; audio.v takes it for its next pass.
//
// Registers (soc.v maps them at 0xF000_5000):
//   0x00 FM_CTRL    W bit 0 run; R the same
//   0x04 FM_WRITE   W a write as above; ignored while the queue is full or run is 0
//   0x08 FM_STATUS  R [8:0] writes in the queue (0..256), bit 16 the chip is ready (out of
//                   reset), bit 17 a write to the chip is still going on
//   0x0C FM_TIME    R [15:0] now
//   0x10 FM_GAIN    W [7:0] gain, 32 = 1 (power-up value)
//

module opl (
    input   wire            clk,
    input   wire            reset,              // the CPU's: run goes to 0

    input   wire            wr,
    input   wire    [4:2]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            run_out,
    output  wire    [8:0]   fill,
    output  wire            ready,
    output  wire            busy,
    output  wire    [15:0]  now,

    input   wire            frame,              // an output frame begins (audio.v)
    output  reg  signed [15:0] fm = 0           // its value, for audio.v's sums
);

//
// the chip's clock enable and the sample counter
//
    localparam [23:0] CEN_NUM = 24'd715909, CEN_DEN = 24'd14850000;
    reg  [23:0] cen_acc = 0;
    reg         cen = 0;
    wire [23:0] cen_sum = cen_acc + CEN_NUM;
always @(posedge clk) begin
    cen <= cen_sum >= CEN_DEN;
    cen_acc <= cen_sum >= CEN_DEN ? cen_sum - CEN_DEN : cen_sum;
end

    reg  [6:0]  sub = 0;                        // the chip's clocks in this sample
    reg  [15:0] now_r = 0;
always @(posedge clk) if(cen) begin
    sub <= sub == 7'd71 ? 7'd0 : sub + 1'b1;
    if(sub == 7'd71) now_r <= now_r + 1'b1;
end
    assign now = now_r;

//
// the queue and the registers
//
    reg         run = 0;
    reg  [7:0]  gain = 8'd32;
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] mem [0:255];
    reg  [8:0]  wptr = 0, rptr = 0;
    reg  [31:0] q;
    wire        full = (wptr - rptr) == 9'd256;
    wire        push = wr && wr_addr == 3'd1 && run && !full;
    reg         take = 0;
    reg  [1:0]  ne = 0;                         // the queue was not empty for two clocks: q is its first
    assign fill = wptr - rptr;
    assign run_out = run;

always @(posedge clk) begin
    if(wr && wr_addr == 3'd0) run <= wr_data[0];
    if(wr && wr_addr == 3'd4) gain <= wr_data[7:0];
    if(reset) run <= 0;

    if(push) mem[wptr[7:0]] <= wr_data;
    q <= mem[rptr[7:0]];
    ne <= {ne[0], wptr != rptr};

    if(!run) begin
        wptr <= 0;
        rptr <= 0;
        ne <= 0;
    end else begin
        if(push) wptr <= wptr + 1'b1;
        if(take) rptr <= rptr + 1'b1;
    end
end

//
// the chip: in reset until 128 of its clocks after run, one write at a time
//
    reg  [7:0]  rst_cnt = 0;
    wire        rst = !rst_cnt[7];
always @(posedge clk) begin
    if(!run) rst_cnt <= 0;
    else if(cen && rst) rst_cnt <= rst_cnt + 1'b1;
end
    assign ready = !rst;

    localparam [2:0] W_IDLE = 0, W_ADDR = 1, W_GAP = 2, W_DATA = 3, W_HOLD = 4;
    reg  [2:0]  wstate = W_IDLE;
    reg  [7:0]  hold = 0;
    reg         bus_wr = 0, bus_a = 0;
    reg  [7:0]  bus_d = 0;
    wire [15:0] late = now_r - q[31:16];        // due when not negative
    wire        due = !late[15];
    assign busy = wstate != W_IDLE;

always @(posedge clk) begin
    take <= 0;
    bus_wr <= 0;
    case(wstate)
        W_IDLE: if(!rst && ne == 2'b11 && due && !take) begin
            bus_wr <= 1;
            bus_a <= 0;
            bus_d <= q[15:8];
            wstate <= W_ADDR;
        end
        W_ADDR: wstate <= W_GAP;
        W_GAP: begin
            bus_wr <= 1;
            bus_a <= 1;
            bus_d <= q[7:0];
            wstate <= W_DATA;
        end
        W_DATA: begin
            take <= 1;
            hold <= 0;
            wstate <= W_HOLD;
        end
        W_HOLD: if(cen) begin
            hold <= hold + 1'b1;
            if(hold[7]) wstate <= W_IDLE;       // 129 enables: 36 us
        end
        default: wstate <= W_IDLE;
    endcase
    if(rst || !run) wstate <= W_IDLE;
end

    wire signed [15:0] chip_snd;
    wire        chip_sample;

jtopl2 chip (
    .rst        ( rst ),
    .clk        ( clk ),
    .cen        ( cen ),
    .din        ( bus_d ),
    .addr       ( bus_a ),
    .cs_n       ( 1'b0 ),
    .wr_n       ( !bus_wr ),
    .dout       ( ),
    .irq_n      ( ),
    .snd        ( chip_snd ),
    .sample     ( chip_sample )
);

//
// the two newest samples (taken when the chip's sample strobe falls, its output is new by then)
// and the frame's value
//
    reg         smp_d = 0;
    reg  signed [15:0] s_new = 0, s_old = 0;
    reg  [10:0] since = 0;                      // clocks since s_new, saturating
    wire [16:0] frac_full = {6'd0, since} * 17'd44;
    wire [16:0] frac = frac_full[16] ? 17'd65535 : frac_full;
always @(posedge clk) begin
    smp_d <= chip_sample;
    if(since != 11'h7FF) since <= since + 1'b1;
    if(smp_d && !chip_sample) begin
        s_old <= s_new;
        s_new <= rst ? 16'sd0 : chip_snd;
        since <= 0;
    end
end

    reg  [3:0]  st = 0;                         // 0 idle, 1.. the steps after `frame`
    reg  signed [17:0] mul_a;
    reg  signed [17:0] mul_b;
    reg  signed [35:0] prod;
    reg  signed [15:0] mid;
    reg  signed [15:0] base;                    // the old sample as it was when the frame began
    wire signed [21:0] scaled = prod[26:5];     // (the interpolated value times the gain) >> 5
always @(posedge clk) begin
    prod <= mul_a * mul_b;
    if(frame) st <= 4'd1;
    else if(st != 4'd0) st <= st == 4'd8 ? 4'd0 : st + 1'b1;
    case(st)
        4'd1: begin                             // new minus old, times the fraction
            mul_a <= {{2{s_new[15]}}, s_new} - {{2{s_old[15]}}, s_old};
            mul_b <= {1'b0, frac};
            base <= s_old;
        end
        4'd4: mid <= base + prod[31:16];
        4'd5: begin                             // times the gain
            mul_a <= {{2{mid[15]}}, mid};
            mul_b <= {10'd0, gain};
        end
        4'd8: fm <= scaled > 22'sd32767 ? 16'sh7FFF : scaled < -22'sd32768 ? 16'sh8000 : scaled[15:0];
        default: ;
    endcase
    if(!ready) fm <= 0;
end

endmodule
