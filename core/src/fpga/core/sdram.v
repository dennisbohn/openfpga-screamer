//
// SDRAM controller for the Pocket's AS4C32M16MSA (64 MB, 16 bit) on clk_74a
// (docs/memory.md)
//
// One job at a time: a read of one 32-bit word or of a 32-byte line (8 words), or a
// write of one 32-bit word with a byte mask. CL2, burst length 2, single-location writes,
// one open row per bank. A READ fetches one word from the even column {addr[10:2], 0};
// its halfwords are taken by the IO input register READ_LATENCY and READ_LATENCY + 1
// clocks after the READ left the IO output register, and the word goes out one clock
// later with the job's tag. A WRITE writes one halfword; halves without mask bits are
// skipped. The next job may start while the data of the previous one is still coming in.
//
// Command spacing, counted in clocks between the edges at which commands leave:
//   ACTIVATE -> READ/WRITE  T_RCD      ACTIVATE -> PRECHARGE (same bank)  T_RAS
//   ACTIVATE -> ACTIVATE    T_RC       PRECHARGE -> ACTIVATE (same bank)  T_RP
//   WRITE -> PRECHARGE      T_WR       READ -> anything but ACTIVATE      2 (else the
//   READ -> WRITE           T_R2W      SDRAM cuts off the burst's second halfword)
//   AUTO REFRESH -> any     T_RFC      LOAD MODE -> any                   T_MRD
//
// Not reset by the core's reset_n: the APF loads data slots while it holds the core in
// reset. The initialisation runs after the SDRAM PLL has locked and again after a loss of
// lock; on request (after the clock phase was moved) a short one with the clock running:
// PRECHARGE ALL, two AUTO REFRESH, the mode registers, while jobs wait. While the SDRAM
// is not ready otherwise, jobs are answered at once: reads with error words, writes
// dropped.
//

module sdram #(
    parameter       READ_LATENCY = 3,           // READ out -> first halfword in the IO register
    parameter       T_RCD = 2,
    parameter       T_RP = 2,
    parameter       T_RAS = 4,
    parameter       T_RC = 5,
    parameter       T_WR = 2,
    parameter       T_R2W = 6,
    parameter       T_RFC = 11,                 // 138 ns
    parameter       T_MRD = 2,
    parameter       INIT_WAIT = 16384,          // NOPs after CKE rises: 220 us > 200 us
    parameter       LOCK_WAIT = 1024,           // clocks of stable lock before CKE rises
    parameter       REFRESH_EVERY = 512,        // 6.9 us; 580 clocks would be 7.8 us
    parameter [12:0] MODE = 13'h0221,           // CL2, BL2, sequential, single-location writes
    parameter [12:0] EXT_MODE = 13'h0040        // full array, drive strength 010 = 25 %
) (
    input   wire            clk,
    input   wire            pll_locked,         // from the SDRAM PLL, asynchronous
    input   wire            reinit,             // one clock: initialise again when idle

    // jobs: taken in a clock with job_valid and job_ready
    input   wire            job_valid,
    output  wire            job_ready,
    input   wire            job_write,
    input   wire            job_line,           // read 8 words from the line of job_addr
    input   wire    [25:2]  job_addr,
    input   wire    [31:0]  job_wdata,
    input   wire    [3:0]   job_mask,
    input   wire    [2:0]   job_tag,            // comes back with every word of a read

    // read words, in job order, one clock each
    output  reg             rd_valid = 0,
    output  reg     [31:0]  rd_data,
    output  reg     [2:0]   rd_tag,
    output  reg             rd_last,            // the job's last word
    output  reg             rd_error,           // answered without the SDRAM

    output  reg             ready = 0,          // initialised, jobs go to the SDRAM
    output  wire            locked,             // pll_locked, synchronised
    output  wire            writing,            // a write job is taken and not yet done
    output  reg     [31:0]  refresh_count = 0,

    // pins; every one straight from a register, so they can sit in the IO cells
    output  reg     [12:0]  dram_a = 0,
    output  reg     [1:0]   dram_ba = 0,
    output  reg     [15:0]  dram_dq_o = 0,
    output  reg             dram_dq_oe = 0,
    input   wire    [15:0]  dram_dq_i,
    output  reg     [1:0]   dram_dqm = 2'b11,
    output  reg             dram_cke = 0,
    output  reg             dram_ras_n = 1,
    output  reg             dram_cas_n = 1,
    output  reg             dram_we_n = 1
);

    localparam  S_OFF   = 3'd0;                 // no clock yet: CKE low
    localparam  S_WAIT  = 3'd1;                 // CKE high, NOPs
    localparam  S_INIT  = 3'd2;                 // PRECHARGE ALL, 2 x REFRESH, MRS, EMRS
    localparam  S_RUN   = 3'd3;

    // {ras_n, cas_n, we_n}
    localparam  [2:0]   C_NOP = 3'b111;
    localparam  [2:0]   C_ACT = 3'b011;
    localparam  [2:0]   C_RD  = 3'b101;
    localparam  [2:0]   C_WR  = 3'b100;
    localparam  [2:0]   C_PRE = 3'b010;
    localparam  [2:0]   C_REF = 3'b001;
    localparam  [2:0]   C_MRS = 3'b000;

    reg  [2:0]  state = S_OFF;
    reg  [1:0]  lock_s = 0;
    reg  [10:0] lock_cnt = 0;
    reg  [14:0] wait_cnt = 0;
    reg  [2:0]  init_step = 0;
    reg         stop = 0;                       // lock lost: power down when idle
    reg         redo = 0;                       // reinit asked: initialise again when idle
    reg         warm = 0;                       // the initialisation runs with the clock on

    assign locked = lock_s[1];

always @(posedge clk) begin
    lock_s <= {lock_s[0], pll_locked};
end

//
// the job being worked on
//
    reg         j_valid = 0;
    reg         j_write;
    reg         j_line;
    reg  [25:2] j_addr;
    reg  [31:0] j_wdata;
    reg  [3:0]  j_mask;
    reg  [2:0]  j_tag;
    reg  [2:0]  j_word;                         // line read: next word
    reg         j_half;                         // write: next halfword

    wire [1:0]  j_bank = j_addr[12:11];
    wire [12:0] j_row = j_addr[25:13];
    wire [9:0]  j_col_rd = {j_addr[10:5], j_line ? j_word : j_addr[4:2], 1'b0};
    wire [9:0]  j_col_wr = {j_addr[10:2], j_half};

    // answers without the SDRAM
    reg  [3:0]  err_left = 0;                   // error words still to send
    reg  [2:0]  err_tag;

    assign writing = j_valid && j_write;

//
// banks and command spacing; counters hold the clocks still to wait
//
    reg  [3:0]  open = 0;
    reg  [12:0] row [0:3];
    reg  [2:0]  ras_cnt [0:3];
    reg  [2:0]  rc_cnt [0:3];
    reg  [1:0]  rcd_cnt [0:3];
    reg  [1:0]  rp_cnt [0:3];
    reg  [1:0]  wr_cnt [0:3];
    reg  [3:0]  busy_cnt = 0;                   // after REFRESH, LOAD MODE, PRECHARGE ALL
    reg         rd_hold = 0;                    // the clock after a READ
    reg  [2:0]  r2w_cnt = 0;

    integer     b;
    initial begin
        for(b = 0; b < 4; b = b + 1) begin
            row[b] = 0;
            ras_cnt[b] = 0;
            rc_cnt[b] = 0;
            rcd_cnt[b] = 0;
            rp_cnt[b] = 0;
            wr_cnt[b] = 0;
        end
    end

    wire        can_pre = ras_cnt[j_bank] == 0 && wr_cnt[j_bank] == 0;
    wire        can_act = rp_cnt[j_bank] == 0 && rc_cnt[j_bank] == 0;
    wire        can_rw = rcd_cnt[j_bank] == 0;
    reg         can_prea;                       // every open bank may be precharged
    reg         all_rp;                         // every bank has finished its precharge
always @(*) begin
    can_prea = 1;
    all_rp = 1;
    for(b = 0; b < 4; b = b + 1) begin
        if(open[b] && (ras_cnt[b] != 0 || wr_cnt[b] != 0)) can_prea = 0;
        if(rp_cnt[b] != 0) all_rp = 0;
    end
end

    // refresh timer
    reg  [9:0]  rf_timer = 0;
    wire        rf_due = rf_timer >= REFRESH_EVERY;

    // which half of a write still has mask bits
    wire        wr_lo = j_mask[1:0] != 2'b00;
    wire        wr_hi = j_mask[3:2] != 2'b00;

    // a job is taken whenever there is room for it; its first command may wait. While the
    // mode registers are written again (warm), jobs wait instead of getting error words.
    assign job_ready = state == S_RUN ? !j_valid && !rf_due && !stop && !redo && err_left == 0
                     : state == S_INIT && warm ? 1'b0
                     : err_left == 0;
    wire        take = state == S_RUN && job_valid && job_ready;

//
// read data: a mark per READ travels READ_LATENCY clocks, then two halfwords come in
//
    reg  [15:0] dq_in;                          // the IO input register
    reg  [15:0] lo;
    reg         mk_valid [0:READ_LATENCY+1];
    reg  [2:0]  mk_tag [0:READ_LATENCY+1];
    reg         mk_last [0:READ_LATENCY+1];
    reg         mk_any;                         // marks still on the way
    integer     m;
    initial begin
        for(m = 0; m <= READ_LATENCY + 1; m = m + 1) mk_valid[m] = 0;
    end
always @(*) begin
    mk_any = 0;
    for(m = 0; m <= READ_LATENCY + 1; m = m + 1) if(mk_valid[m]) mk_any = 1;
end

//
// the command for the next clock edge
//
    reg  [2:0]  cmd;
    reg  [1:0]  cmd_ba;
    reg  [12:0] cmd_a;
    reg         cmd_wr_data;                    // drive the write halfword
    reg         mark;                           // a READ goes out: start a mark

always @(*) begin
    cmd = C_NOP;
    cmd_ba = dram_ba;
    cmd_a = dram_a;
    cmd_wr_data = 0;
    mark = 0;

    if(state == S_INIT && busy_cnt == 0 && !rd_hold) begin
        case(init_step)
        3'd0: if(can_prea) begin cmd = C_PRE; cmd_a = 13'h0400; end    // PRECHARGE ALL (A10)
        3'd1, 3'd2: begin
            if(all_rp) cmd = C_REF;
        end
        3'd3: begin cmd = C_MRS; cmd_ba = 2'b00; cmd_a = MODE; end
        3'd4: begin cmd = C_MRS; cmd_ba = 2'b10; cmd_a = EXT_MODE; end
        default: ;
        endcase
    end else if(state == S_RUN && busy_cnt == 0 && !rd_hold) begin
        if(j_valid) begin
            if(!open[j_bank]) begin
                if(can_act) begin
                    cmd = C_ACT; cmd_ba = j_bank; cmd_a = j_row;
                end
            end else if(row[j_bank] != j_row) begin
                if(can_pre) begin
                    cmd = C_PRE; cmd_ba = j_bank; cmd_a = 13'h0000;
                end
            end else if(can_rw) begin
                if(!j_write) begin
                    cmd = C_RD; cmd_ba = j_bank; cmd_a = {3'b000, j_col_rd};
                    mark = 1;
                end else if(r2w_cnt == 0) begin
                    cmd = C_WR; cmd_ba = j_bank; cmd_a = {3'b000, j_col_wr};
                    cmd_wr_data = 1;
                end
            end
        end else if(rf_due) begin
            if(open != 0) begin
                if(can_prea) begin cmd = C_PRE; cmd_a = 13'h0400; end
            end else if(all_rp) begin
                cmd = C_REF;
            end
        end
    end
end

always @(posedge clk) begin
    // defaults: NOP, data bus released, DQM low except during init
    {dram_ras_n, dram_cas_n, dram_we_n} <= cmd;
    dram_ba <= cmd_ba;
    dram_a <= cmd_a;
    dram_dq_oe <= cmd_wr_data;
    dram_dqm <= state == S_RUN ? 2'b00 : 2'b11;
    if(cmd_wr_data) begin
        dram_dq_o <= j_half ? j_wdata[31:16] : j_wdata[15:0];
        dram_dqm <= j_half ? ~j_mask[3:2] : ~j_mask[1:0];
    end

    // counters run down
    for(b = 0; b < 4; b = b + 1) begin
        if(ras_cnt[b] != 0) ras_cnt[b] <= ras_cnt[b] - 1'b1;
        if(rc_cnt[b] != 0) rc_cnt[b] <= rc_cnt[b] - 1'b1;
        if(rcd_cnt[b] != 0) rcd_cnt[b] <= rcd_cnt[b] - 1'b1;
        if(rp_cnt[b] != 0) rp_cnt[b] <= rp_cnt[b] - 1'b1;
        if(wr_cnt[b] != 0) wr_cnt[b] <= wr_cnt[b] - 1'b1;
    end
    if(busy_cnt != 0) busy_cnt <= busy_cnt - 1'b1;
    if(r2w_cnt != 0) r2w_cnt <= r2w_cnt - 1'b1;
    rd_hold <= cmd == C_RD;

    // what the command does to the banks and the job
    case(cmd)
    C_ACT: begin
        open[cmd_ba] <= 1;
        row[cmd_ba] <= cmd_a;
        ras_cnt[cmd_ba] <= T_RAS - 1;
        rc_cnt[cmd_ba] <= T_RC - 1;
        rcd_cnt[cmd_ba] <= T_RCD - 1;
    end
    C_PRE: begin
        if(cmd_a[10]) begin
            open <= 0;
            for(b = 0; b < 4; b = b + 1) rp_cnt[b] <= T_RP - 1;
        end else begin
            open[cmd_ba] <= 0;
            rp_cnt[cmd_ba] <= T_RP - 1;
        end
    end
    C_RD: begin
        r2w_cnt <= T_R2W - 1;
        if(!j_line || j_word == 3'd7) j_valid <= 0;
        j_word <= j_word + 1'b1;
    end
    C_WR: begin
        wr_cnt[cmd_ba] <= T_WR - 1;
        j_half <= 1;
        if(j_half || !wr_hi) j_valid <= 0;
    end
    C_REF: begin
        busy_cnt <= T_RFC - 1;
        refresh_count <= refresh_count + 1'b1;
        if(state == S_RUN) rf_timer <= 0;
    end
    C_MRS: busy_cnt <= T_MRD - 1;
    default: ;
    endcase
    if(state == S_RUN && !(cmd == C_REF) && !rf_due) rf_timer <= rf_timer + 1'b1;

    // a new job
    if(take) begin
        j_valid <= 1;
        j_write <= job_write;
        j_line <= job_line;
        j_addr <= job_addr;
        j_wdata <= job_wdata;
        j_mask <= job_mask;
        j_tag <= job_tag;
        j_word <= 0;
        j_half <= job_mask[1:0] == 2'b00;   // a write with only the upper half
    end

    // marks and read data
    mk_valid[0] <= mark;
    mk_tag[0] <= j_tag;
    mk_last[0] <= !j_line || j_word == 3'd7;
    for(m = 1; m <= READ_LATENCY + 1; m = m + 1) begin
        mk_valid[m] <= mk_valid[m-1];
        mk_tag[m] <= mk_tag[m-1];
        mk_last[m] <= mk_last[m-1];
    end
    dq_in <= dram_dq_i;
    if(mk_valid[READ_LATENCY]) lo <= dq_in;

    rd_valid <= 0;
    if(mk_valid[READ_LATENCY+1]) begin
        rd_valid <= 1;
        rd_data <= {dq_in, lo};
        rd_tag <= mk_tag[READ_LATENCY+1];
        rd_last <= mk_last[READ_LATENCY+1];
        rd_error <= 0;
    end else if(err_left != 0 && !mk_any) begin
        rd_valid <= 1;
        rd_data <= 32'd0;
        rd_tag <= err_tag;
        rd_last <= err_left == 4'd1;
        rd_error <= 1;
        err_left <= err_left - 1'b1;
    end

    // jobs while the SDRAM is not ready: reads get error words, writes are dropped
    if(state != S_RUN && job_valid && job_ready && !job_write) begin
        err_left <= job_line ? 4'd8 : 4'd1;
        err_tag <= job_tag;
    end

    // initialisation and lock: a lost lock powers the SDRAM down and up again; a reinit
    // (after the clock phase was moved) keeps the clock and CKE and writes PRECHARGE ALL,
    // two AUTO REFRESH and the mode registers again
    if(!lock_s[1]) stop <= 1;
    if(reinit) redo <= 1;
    case(state)
    S_OFF: begin
        dram_cke <= 0;
        ready <= 0;
        open <= 0;
        stop <= 0;
        warm <= 0;
        if(!lock_s[1]) lock_cnt <= 0;
        else if(lock_cnt != LOCK_WAIT) lock_cnt <= lock_cnt + 1'b1;
        else begin
            state <= S_WAIT;
            wait_cnt <= INIT_WAIT - 1;
        end
    end
    S_WAIT: begin
        dram_cke <= 1;
        if(!lock_s[1]) state <= S_OFF;
        else if(wait_cnt != 0) wait_cnt <= wait_cnt - 1'b1;
        else begin
            state <= S_INIT;
            init_step <= 0;
        end
    end
    S_INIT: begin
        if(!lock_s[1]) state <= S_OFF;
        else if(cmd != C_NOP) begin
            init_step <= init_step + 1'b1;
        end else if(init_step == 3'd5 && busy_cnt == 0) begin
            state <= S_RUN;
            ready <= 1;
            warm <= 0;
            rf_timer <= 0;
        end
    end
    S_RUN: begin
        if(stop && !j_valid && !mk_any) begin
            state <= S_OFF;
            ready <= 0;
            dram_cke <= 0;
            lock_cnt <= 0;
        end else if(redo && !j_valid && !mk_any) begin
            state <= S_INIT;
            init_step <= 0;
            warm <= 1;
            redo <= 0;
            ready <= 0;
        end
    end
    default: state <= S_OFF;
    endcase
end

endmodule
