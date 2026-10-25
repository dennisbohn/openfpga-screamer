//
// Behavioural model of the Pocket's SDRAM, Alliance AS4C32M16MSA-6BIN: 1.8 V mobile SDR SDRAM,
// 32M x 16 = 4 banks x 8192 rows x 1024 columns (64 MiB), /CS strapped low on the board. For
// cycle-based simulation with Verilator only: no delays, no real time, no tristate. It stores
// the data and checks the controller against the datasheet (Rev 1.0 "Preliminary", Dec 2017;
// "p.n" below is its page n of 46).
//
// CYCLE CONVENTION
//   Every input is sampled at posedge clk; the outputs change at posedge clk (non-blocking).
//   "Edge n" is the n-th rising edge the model sees, the first being edge 0; error messages
//   print it as "clock n". A controller register that launches a command at its edge L puts
//   it in front of the model at edge r = L + 1 (on the board the chip samples it ~0.74 clock
//   after L).
//   READ registered at edge r: beat k is driven from edge r+CL-1+k for one clock (dq_o and
//     dq_o_en change at that edge), so it is valid at edge r+CL+k: a controller register that
//     samples dq at its launch edge L + CL + 1 + k captures beat k.
//   WRITE registered at edge r: beat k is taken from dq_i at edge r+k (only k = 0 when A9 = 1,
//     single-location writes). dq_i_en high at edge n means the controller drove DQ in the
//     clock that ends at edge n.
//   DQM: write latency 0 (high at a data edge masks that byte), read latency 2 (high at edge n
//     masks the beat valid at edge n+2). dqm[0] = LDQM = DQ7:0, dqm[1] = UDQM = DQ15:8.
//   The testbench resolves the bus from dq_i/dq_i_en and dq_o/dq_o_en. When the chip does not
//   drive a beat that a READ would have delivered (masked by DQM, or lost because the burst
//   was cut short), dq_o carries the bitwise inverse of that beat's data (per byte lane for a
//   lane masked by DQM) and dq_o_en is low unless the other lane is driven; with no beat at
//   all dq_o is 0. Such poison never equals the stored data.
//
// BUS OWNERSHIP ("bus_conflict")
//   The chip owns DQ from the clock after the edge that registers a READ (dq_i_en sampled at
//   r+1, r+2, ...) until BUS_HOLD clocks after the last beat ends (last beat valid at edge e:
//   owned through edge e+BUS_HOLD), whether or not DQM masks beats. Back-to-back READs merge
//   into one window; PRECHARGE or BURST TERMINATE end it CL-1 clocks after them plus
//   BUS_HOLD, a WRITE BUS_HOLD clocks after the WRITE. Any dq_i_en inside the window is an
//   error (one per window). This is stricter than the datasheet, whose Figures 6-8 let write
//   data follow read data after 0-2 clocks: in this cycle model the chip registers a command
//   one clock after the controller launched it, while on the board it sees it ~0.74 clock
//   after launch and starts driving read data ~0.74 clock earlier than this model shows.
//   With BUS_HOLD = 2 a WRITE may come 3 clocks after the edge at which the last beat is valid.
//
// MEMORY
//   Sparse (associative array keyed by {bank, row, column}). A word never written reads as
//   16'hA5A5 ^ key[15:0] ^ {key[24:16], 7'b0}, key = {bank[1:0], row[12:0], column[9:0]}.
//
// MODEL OF THE CHIP
//   Mode register (BA = 00): A9 write burst mode (1 = single-location writes), A8:A7 = 00,
//   A6:A4 CAS latency (010 = 2, 011 = 3; CL1 and the rest are treated as illegal), A3 burst
//   type, A2:A0 burst length 1/2/4/8 or 111 = full page (sequential only), A12:A10 = 0.
//   Bursts wrap inside their aligned block (p.7-8 Table 2); a full page wraps in the row and
//   runs until a READ, WRITE, BURST TERMINATE or PRECHARGE ends it. Extended mode register
//   (BA = 10): A2:A0 PASR (000/001/010/101/110), A7:A5 drive strength (000..100), A4:A3 and
//   A12:A8 = 0. Both are printed when written.
//   A READ, WRITE, BURST TERMINATE or PRECHARGE of the burst's bank ends a running burst
//   (p.18-34): read beats fetched before that edge still come out, so after PRECHARGE/BURST
//   TERMINATE the chip delivers CL-1 more beats; a WRITE also stops the read beats due after
//   its edge; write data on the edge of the interrupting command is not written.
//   Auto precharge (A10 = 1, not in full-page mode): after a READ the bank starts precharging
//   at edge r+BL (the earliest edge a PRECHARGE would not cut the burst), after a WRITE tWR
//   after its last data edge; a READ or WRITE to another bank that cuts the burst starts it at
//   that edge (READ burst) or tWR after it (WRITE burst) (p.38-40, notes 81-84). The bank is
//   idle tRP after the precharge starts. The datasheet documents no tRAS lock-out, so an auto
//   precharge that would start before tRAS is an error.
//
// CHECKS (error tag in brackets)
//   Each error increments error_count, sets the sticky error output and prints one line
//   "sdram_model: clock N: ERROR [tag] message" (only the first MAX_MSG lines); FATAL = 1
//   stops the simulation at the first one. Nothing is checked before the first edge with CKE
//   high (edge t0, the start of power-up).
//   [cke]               CKE low after t0 (power-down/self refresh/clock suspend not modelled)
//   [init_wait]         a command other than NOP before T_INIT_NS (200 us) after t0
//   [dqm_init]          DQM not 11 between t0 and the end of the initialisation (p.6 Fig. 1)
//   [init_order]        ACTIVE/READ/WRITE before two AUTO REFRESH, a legal LOAD MODE REGISTER
//                       and a legal LOAD EXTENDED MODE REGISTER, all after the 200 us wait
//   [not_idle]          AUTO REFRESH or LOAD MODE REGISTER while a bank is open, has an auto
//                       precharge pending or was never precharged since power-up
//   [tRP]               ACTIVE, AUTO REFRESH or LOAD MODE REGISTER within tRP of a precharge
//   [tRCD] [tRAS] [tRC] [tRRD] [tWR] [tRFC] [tMRD]  minimum times (ns rounded up to clocks;
//                       tRFC and tMRD block every command, p.42 note 61); tCCD = 1 clock is
//                       always met
//   [tRAS_max]          a row open longer than T_RAS_MAX_NS (rounded down)
//   [bank_open]         ACTIVE to a bank whose row is open
//   [bank_closed]       READ/WRITE to a bank with no open row
//   [auto_precharge]    a command to a bank during its READ/WRITE with auto precharge (until
//                       tRP after the precharge starts, p.42 note 60), or A10 = 1 in full page
//   [precharge_busy]    PRECHARGE of a bank that is still precharging (p.42 notes 60, 64)
//   [precharge_truncate] PRECHARGE that cuts a WRITE burst while DQM is low on its edge (p.31)
//   [burst_terminate]   BURST TERMINATE with no burst in progress, or of an auto-precharge burst
//   [truncation]        (TRUNC_IS_ERROR) a fixed-length READ or WRITE burst cut short by
//                       READ, WRITE, BURST TERMINATE or PRECHARGE; full-page bursts excepted
//   [read_mask]         (READ_MASK_IS_ERROR) a read beat masked by DQM
//   [bus_conflict]      dq_i_en inside the chip's bus window (see above)
//   [write_undriven]    an unmasked write beat with dq_i_en low
//   [mode_reg] [ext_mode_reg]  reserved or illegal field values, or LOAD MODE with BA = 01/11
//   [cl_clock]          CL2 with CLK_NS < 12 (83 MHz), CL3 with CLK_NS < 6 (166 MHz)
//   [refresh_interval]  two AUTO REFRESH (from the first one on) more than
//                       T_REFI_NS x REF_FACTOR apart (rounded down); max_refresh_interval
//                       holds the longest interval so far, the running one included
//
// DATASHEET READINGS
//   tRFC is 80 ns in the AC table but 138 ns in the IDD5 condition and "tRC" in note 61: the
//   AC table is used, T_RFC_NS raises it. tRC can never be the only violated limit with the
//   datasheet values (tRAS + tRP = 66 ns > 60 ns). Note 4 says "y = 512" for a full page; the
//   column address is A9:A0, so a page is 1024 words. The EMR text says "M7 through M12 set to
//   0" but Table 5 uses A7 for drive strength: Table 5 wins (A7:A5 = 100 = 75 % is legal).
//   p.22 "DQM must be asserted prior to the WRITE" contradicts write latency 0 (typo). CL1 is
//   allowed by the datasheet up to 50 MHz but treated as illegal here, as briefed.
//   The datasheet only requires "a PRECHARGE" after 200 us with all banks then precharged:
//   any PRECHARGE that reaches every bank works. 200 us at CLK_NS = 13.468 is 14851 clocks
//   (14850 x 13.468 ns = 199999.8 ns); exactly 74.25 MHz gives 14850 (CLK_NS = 13.4680135).
//
// PARAMETERS
//   CLK_NS             clock period; every ns limit is converted to clocks with it
//   T_RCD_NS .. T_RFC_NS, T_MRD_CLK, T_INIT_NS, T_REFI_NS   the limits (datasheet defaults)
//   REF_FACTOR         the refresh interval check allows T_REFI_NS x REF_FACTOR
//   BUS_HOLD           clocks the chip keeps the bus after its last read beat (default 2)
//   TRUNC_IS_ERROR     a burst cut short is an error (default 1; the controller never does it)
//   READ_MASK_IS_ERROR a read beat masked by DQM is an error (default 1)
//   FATAL              stop at the first error ($fatal) (default 0)
//   MAX_MSG            error lines printed (default 50); error_count counts all
//   Fault injection for negative tests of a memory test, all off by default; each can also
//   be set with a build define of the same name prefixed SDRAM_ (for example
//   +define+SDRAM_FAULT_DQM_SWAP=1). The faults act on the data path and the array address
//   only: the protocol checks see the pins as driven, so a fault never causes a protocol
//   error by itself.
//   FAULT_DQ_STUCK0/1  16-bit masks of DQ lines stuck at 0/1 (written and read data)
//   FAULT_DQ_SHORT_A/B two DQ lines shorted, wired-AND (written and read data); -1 = off
//   FAULT_LDQM_LOW     the chip sees LDQM low (byte masks of DQ7:0 ignored)
//   FAULT_UDQM_LOW     the chip sees UDQM low (byte masks of DQ15:8 ignored)
//   FAULT_DQM_SWAP     dqm[0] reaches UDQM and dqm[1] LDQM
//   FAULT_BA1_STUCK0   the array sees BA1 = 0: banks 2 and 3 alias banks 0 and 1
//   FAULT_ROW_ALIAS_SRC/DST  the array's row bit DST is taken from row bit SRC (-1 = off)
//   FAULT_DROP_WRITE_N every Nth write beat that stores anything is dropped (0 = off)
//

`ifndef SDRAM_FAULT_DQ_STUCK0
`define SDRAM_FAULT_DQ_STUCK0 16'h0000
`endif
`ifndef SDRAM_FAULT_DQ_STUCK1
`define SDRAM_FAULT_DQ_STUCK1 16'h0000
`endif
`ifndef SDRAM_FAULT_DQ_SHORT_A
`define SDRAM_FAULT_DQ_SHORT_A -1
`endif
`ifndef SDRAM_FAULT_DQ_SHORT_B
`define SDRAM_FAULT_DQ_SHORT_B -1
`endif
`ifndef SDRAM_FAULT_LDQM_LOW
`define SDRAM_FAULT_LDQM_LOW 0
`endif
`ifndef SDRAM_FAULT_UDQM_LOW
`define SDRAM_FAULT_UDQM_LOW 0
`endif
`ifndef SDRAM_FAULT_DQM_SWAP
`define SDRAM_FAULT_DQM_SWAP 0
`endif
`ifndef SDRAM_FAULT_BA1_STUCK0
`define SDRAM_FAULT_BA1_STUCK0 0
`endif
`ifndef SDRAM_FAULT_ROW_ALIAS_SRC
`define SDRAM_FAULT_ROW_ALIAS_SRC -1
`endif
`ifndef SDRAM_FAULT_ROW_ALIAS_DST
`define SDRAM_FAULT_ROW_ALIAS_DST -1
`endif
`ifndef SDRAM_FAULT_DROP_WRITE_N
`define SDRAM_FAULT_DROP_WRITE_N 0
`endif

module sdram_model #(
    parameter real    CLK_NS              = 13.468,     // 74.25 MHz
    parameter real    T_RCD_NS            = 18.0,
    parameter real    T_RP_NS             = 18.0,
    parameter real    T_RAS_NS            = 48.0,
    parameter real    T_RAS_MAX_NS        = 100000.0,
    parameter real    T_RC_NS             = 60.0,
    parameter real    T_RRD_NS            = 12.0,
    parameter real    T_WR_NS             = 15.0,
    parameter real    T_RFC_NS            = 80.0,
    parameter integer T_MRD_CLK           = 2,
    parameter real    T_INIT_NS           = 200000.0,
    parameter real    T_REFI_NS           = 7812.5,     // 64 ms / 8192
    parameter real    REF_FACTOR          = 1.0,
    parameter integer BUS_HOLD            = 2,
    parameter integer TRUNC_IS_ERROR      = 1,
    parameter integer READ_MASK_IS_ERROR  = 1,
    parameter integer FATAL               = 0,
    parameter integer MAX_MSG             = 50,
    parameter [15:0]  FAULT_DQ_STUCK0     = `SDRAM_FAULT_DQ_STUCK0,
    parameter [15:0]  FAULT_DQ_STUCK1     = `SDRAM_FAULT_DQ_STUCK1,
    parameter integer FAULT_DQ_SHORT_A    = `SDRAM_FAULT_DQ_SHORT_A,
    parameter integer FAULT_DQ_SHORT_B    = `SDRAM_FAULT_DQ_SHORT_B,
    parameter integer FAULT_LDQM_LOW      = `SDRAM_FAULT_LDQM_LOW,
    parameter integer FAULT_UDQM_LOW      = `SDRAM_FAULT_UDQM_LOW,
    parameter integer FAULT_DQM_SWAP      = `SDRAM_FAULT_DQM_SWAP,
    parameter integer FAULT_BA1_STUCK0    = `SDRAM_FAULT_BA1_STUCK0,
    parameter integer FAULT_ROW_ALIAS_SRC = `SDRAM_FAULT_ROW_ALIAS_SRC,
    parameter integer FAULT_ROW_ALIAS_DST = `SDRAM_FAULT_ROW_ALIAS_DST,
    parameter integer FAULT_DROP_WRITE_N  = `SDRAM_FAULT_DROP_WRITE_N
) (
    input   wire            clk,
    input   wire            cke,
    input   wire            ras_n,
    input   wire            cas_n,
    input   wire            we_n,
    input   wire    [1:0]   ba,
    input   wire    [12:0]  a,
    input   wire    [1:0]   dqm,
    input   wire    [15:0]  dq_i,                   // what the controller drives
    input   wire            dq_i_en,                // the controller drives DQ this clock
    output  reg     [15:0]  dq_o = 16'h0000,        // what the chip drives
    output  reg             dq_o_en = 1'b0,         // the chip drives DQ this clock
    output  reg             error = 1'b0,           // sticky
    output  reg     [31:0]  error_count = 32'd0,
    output  reg             init_done = 1'b0,       // initialised, both mode registers legal
    output  reg     [31:0]  max_refresh_interval = 32'd0,   // clocks
    output  reg     [31:0]  refresh_count = 32'd0
);

    // ns -> clocks: rounded up for minimum times, down for maximum times
    function automatic integer ns_up(input real ns, input real clk_ns);
        integer n;
        begin
            n = $rtoi(ns / clk_ns);
            if (n * clk_ns < ns) n = n + 1;
            ns_up = n;
        end
    endfunction

    function automatic integer ns_down(input real ns, input real clk_ns);
        ns_down = $rtoi(ns / clk_ns);
    endfunction

    localparam integer NEVER    = -(1 << 28);   // "long ago": every limit counted from it is met
    localparam integer FOREVER  = 1 << 30;
    localparam integer TRCD     = ns_up(T_RCD_NS, CLK_NS);
    localparam integer TRP      = ns_up(T_RP_NS, CLK_NS);
    localparam integer TRAS     = ns_up(T_RAS_NS, CLK_NS);
    localparam integer TRAS_MAX = ns_down(T_RAS_MAX_NS, CLK_NS);
    localparam integer TRC      = ns_up(T_RC_NS, CLK_NS);
    localparam integer TRRD     = ns_up(T_RRD_NS, CLK_NS);
    localparam integer TWR      = ns_up(T_WR_NS, CLK_NS);
    localparam integer TRFC     = ns_up(T_RFC_NS, CLK_NS);
    localparam integer TMRD     = T_MRD_CLK;
    localparam integer TINIT    = ns_up(T_INIT_NS, CLK_NS);
    localparam integer TREF_MAX = ns_down(T_REFI_NS * REF_FACTOR, CLK_NS);

    // the faults, with their indices clamped so that a disabled fault elaborates cleanly
    localparam integer SH_ON  = (FAULT_DQ_SHORT_A >= 0 && FAULT_DQ_SHORT_A <= 15 && FAULT_DQ_SHORT_B >= 0
                                 && FAULT_DQ_SHORT_B <= 15 && FAULT_DQ_SHORT_A != FAULT_DQ_SHORT_B) ? 1 : 0;
    localparam integer SH_A   = SH_ON != 0 ? FAULT_DQ_SHORT_A : 0;
    localparam integer SH_B   = SH_ON != 0 ? FAULT_DQ_SHORT_B : 1;
    localparam integer RA_ON  = (FAULT_ROW_ALIAS_SRC >= 0 && FAULT_ROW_ALIAS_SRC <= 12 && FAULT_ROW_ALIAS_DST >= 0
                                 && FAULT_ROW_ALIAS_DST <= 12) ? 1 : 0;
    localparam integer RA_SRC = RA_ON != 0 ? FAULT_ROW_ALIAS_SRC : 0;
    localparam integer RA_DST = RA_ON != 0 ? FAULT_ROW_ALIAS_DST : 0;
    localparam integer DROP_N = FAULT_DROP_WRITE_N > 0 ? FAULT_DROP_WRITE_N : 1;

    // The model is one process that runs once per rising edge as a sequential program: its
    // state is read and written only there, with blocking assignments; only the outputs
    // leave it, through non-blocking assignments.
    /* verilator lint_off BLKSEQ */

    logic   [15:0]  mem [int unsigned];         // the array, sparse

    integer         cyc = 0;                    // the number of the current edge
    integer         nerr = 0;
    reg             started = 1'b0;             // CKE was high: power-up has begun
    integer         t_start = 0;                // that edge (t0)
    reg             cke_low = 1'b0;
    reg             dqm_flagged = 1'b0;

    // banks
    reg             b_open      [0:3];          // a row is open (until a PRECHARGE or auto precharge)
    reg             b_known     [0:3];          // precharged at least once since power-up
    reg     [12:0]  b_row       [0:3];
    integer         b_act       [0:3];          // edge of the last ACTIVE
    integer         b_pre       [0:3];          // edge at which the last precharge started
    reg             b_pre_ap    [0:3];          // ... and it was an auto precharge
    integer         b_wr        [0:3];          // edge of the last unmasked write data in the open row
    reg             b_ap        [0:3];          // auto precharge pending (READ/WRITE with A10 = 1)
    integer         b_ap_at     [0:3];          // edge at which it starts
    integer         b_ap_cmd    [0:3];          // edge of that READ/WRITE
    reg             b_rasmax    [0:3];          // tRAS max already reported for this ACTIVE

    // initialisation and mode registers
    integer         ar_init = 0;                // AUTO REFRESH after the 200 us wait
    reg             mr_ok = 1'b0;               // a legal LOAD MODE REGISTER after the wait
    reg             emr_ok = 1'b0;              // a legal LOAD EXTENDED MODE REGISTER after the wait
    reg             init_ok = 1'b0;
    reg             init_seen = 1'b0;           // init_ok was high once
    integer         cl = 2;
    integer         bl = 1;                     // 0 = full page
    reg             il = 1'b0;                  // interleaved burst type
    reg             wb_single = 1'b0;           // A9: single-location writes

    // command history
    integer         t_ref = NEVER;              // last AUTO REFRESH
    integer         t_mrs = NEVER;              // last LOAD MODE REGISTER
    reg             ref_late = 1'b0;            // the running refresh interval was reported
    integer         ref_max = 0;
    integer         ref_cnt = 0;

    // the read burst: the internal column counter fetches one beat per edge, the beat comes
    // out CL clocks later; after a cut its remaining beats are fetched as lost (ghost)
    reg             rd_on = 1'b0;
    reg             rd_ghost = 1'b0;
    reg     [1:0]   rd_bank = 2'd0;
    reg     [12:0]  rd_row = 13'd0;
    reg     [9:0]   rd_col = 10'd0;
    integer         rd_k = 0;                   // next beat
    integer         rd_bl = 1;                  // 0 = full page
    integer         rd_r = 0;                   // edge of the READ
    reg             rd_ap = 1'b0;

    // the write burst
    reg             wr_on = 1'b0;
    reg     [1:0]   wr_bank = 2'd0;
    reg     [12:0]  wr_row = 13'd0;
    reg     [9:0]   wr_col = 10'd0;
    integer         wr_k = 0;
    integer         wr_bl = 1;
    integer         wr_w = 0;                   // edge of the WRITE
    reg             wr_ap = 1'b0;
    reg             last_ap = 1'b0;             // the most recent READ/WRITE had auto precharge
    integer         wr_count = 0;               // write beats that stored something (fault)

    // read beats on their way out, slot = edge at which they are valid, modulo 4
    reg             p_v         [0:3];          // a beat is due
    reg             p_drv       [0:3];          // ... and not lost
    reg     [15:0]  p_d         [0:3];
    reg     [24:0]  p_addr      [0:3];          // {bank, row, column} for messages
    integer         p_r         [0:3];          // edge of its READ
    reg     [1:0]   dqm_d1 = 2'b11;             // DQM at the previous edge

    // the chip's bus window
    integer         own_from = NEVER;
    integer         own_until = NEVER;
    integer         own_read = NEVER;           // the READ that set it
    reg             conflict_seen = 1'b0;

    // temporaries of the edge process
    integer         e, i, j, s, lost, first;
    reg             pend;
    reg     [2:0]   cmd;
    reg     [9:0]   col;
    reg     [24:0]  key;
    reg     [15:0]  old, din, v;
    reg     [1:0]   m;

    initial begin
        for (i = 0; i < 4; i = i + 1) begin
            b_open[i] = 1'b0;
            b_known[i] = 1'b0;
            b_row[i] = 13'd0;
            b_act[i] = NEVER;
            b_pre[i] = NEVER;
            b_pre_ap[i] = 1'b0;
            b_wr[i] = NEVER;
            b_ap[i] = 1'b0;
            b_ap_at[i] = NEVER;
            b_ap_cmd[i] = NEVER;
            b_rasmax[i] = 1'b0;
            p_v[i] = 1'b0;
            p_drv[i] = 1'b0;
            p_d[i] = 16'h0000;
            p_addr[i] = 25'd0;
            p_r[i] = NEVER;
        end
    end

    // the column of beat k of a burst from column c (len 0 = full page; p.7-8 Table 2)
    function automatic [9:0] burst_col(input [9:0] c, input integer k, input integer len, input inter);
        reg [9:0] kk;
        reg [9:0] msk;
        begin
            kk = k[9:0];
            if (len == 0) burst_col = c + kk;
            else begin
                msk = len[9:0] - 10'd1;
                if (inter) burst_col = (c & ~msk) | ((c ^ kk) & msk);
                else       burst_col = (c & ~msk) | ((c + kk) & msk);
            end
        end
    endfunction

    // what crosses the DQ lines: the stuck lines, then the short (wired-AND)
    function automatic [15:0] dq_fault(input [15:0] x);
        reg [15:0] r;
        reg        w;
        begin
            r = (x & ~FAULT_DQ_STUCK0) | FAULT_DQ_STUCK1;
            if (SH_ON != 0) begin
                w = r[SH_A] & r[SH_B];
                r[SH_A] = w;
                r[SH_B] = w;
            end
            dq_fault = r;
        end
    endfunction

    // the DQM the chip's data paths see
    function automatic [1:0] dqm_fault(input [1:0] x);
        reg [1:0] r;
        begin
            r = FAULT_DQM_SWAP != 0 ? {x[0], x[1]} : x;
            if (FAULT_LDQM_LOW != 0) r[0] = 1'b0;
            if (FAULT_UDQM_LOW != 0) r[1] = 1'b0;
            dqm_fault = r;
        end
    endfunction

    // where a word lives in the array, after the address faults
    function automatic [24:0] array_key(input [1:0] bank, input [12:0] row, input [9:0] c);
        reg [1:0]  bk;
        reg [12:0] rw;
        begin
            bk = bank;
            rw = row;
            if (FAULT_BA1_STUCK0 != 0) bk[1] = 1'b0;
            if (RA_ON != 0) rw[RA_DST] = row[RA_SRC];
            array_key = {bk, rw, c};
        end
    endfunction

    function automatic [15:0] mem_rd(input [24:0] k);
        if (mem.exists({7'd0, k}) != 0) mem_rd = mem[{7'd0, k}];
        else mem_rd = 16'hA5A5 ^ k[15:0] ^ {k[24:16], 7'd0};
    endfunction

    function automatic string cmd_name(input [2:0] c);
        case (c)
            3'b111:  cmd_name = "NOP";
            3'b011:  cmd_name = "ACTIVE";
            3'b101:  cmd_name = "READ";
            3'b100:  cmd_name = "WRITE";
            3'b110:  cmd_name = "BURST TERMINATE";
            3'b010:  cmd_name = "PRECHARGE";
            3'b001:  cmd_name = "AUTO REFRESH";
            default: cmd_name = "LOAD MODE REGISTER";
        endcase
    endfunction

    // c ? t : f on strings (a ?: between two string literals pads the shorter one)
    function automatic string pick(input logic c, input string t, input string f);
        pick = c ? t : f;
    endfunction

    task automatic report(input string tag, input string msg);
        begin
            nerr = nerr + 1;
            if (nerr <= MAX_MSG) $display("sdram_model: clock %0d: ERROR [%s] %s", cyc, tag, msg);
            if (nerr == MAX_MSG) $display("sdram_model: clock %0d: %0d errors, no further error lines", cyc, nerr);
            if (FATAL != 0) $fatal(1, "sdram_model: FATAL = 1, stopping at the first error");
        end
    endtask

    function automatic string init_missing();
        string t;
        begin
            t = "";
            if (ar_init < 2) t = {t, $sformatf(" %0d more AUTO REFRESH,", 2 - ar_init)};
            if (!mr_ok) t = {t, " a legal LOAD MODE REGISTER,"};
            if (!emr_ok) t = {t, " a legal LOAD EXTENDED MODE REGISTER,"};
            init_missing = {t, " each after the 200 us wait"};
        end
    endfunction

    task automatic startup_message();
        string f;
        begin
            $display("sdram_model: clock %0d: CKE high, power-up starts. CLK_NS %0.4f: tRCD %0d, tRP %0d, tRAS %0d..%0d, tRC %0d, tRRD %0d, tWR %0d, tRFC %0d, tMRD %0d clocks; first command after %0d clocks; AUTO REFRESH at least every %0d clocks; bus held %0d clocks after the last read beat",
                     cyc, CLK_NS, TRCD, TRP, TRAS, TRAS_MAX, TRC, TRRD, TWR, TRFC, TMRD, TINIT, TREF_MAX, BUS_HOLD);
            f = "";
            if (FAULT_DQ_STUCK0 != 16'h0000) f = {f, $sformatf(" DQ stuck at 0 (mask 0x%04x);", FAULT_DQ_STUCK0)};
            if (FAULT_DQ_STUCK1 != 16'h0000) f = {f, $sformatf(" DQ stuck at 1 (mask 0x%04x);", FAULT_DQ_STUCK1)};
            if (SH_ON != 0) f = {f, $sformatf(" DQ%0d and DQ%0d shorted (wired-AND);", SH_A, SH_B)};
            if (FAULT_LDQM_LOW != 0) f = {f, " LDQM seen low;"};
            if (FAULT_UDQM_LOW != 0) f = {f, " UDQM seen low;"};
            if (FAULT_DQM_SWAP != 0) f = {f, " DQML and DQMH swapped;"};
            if (FAULT_BA1_STUCK0 != 0) f = {f, " BA1 stuck at 0 in the array;"};
            if (RA_ON != 0) f = {f, $sformatf(" row bit %0d aliased onto row bit %0d;", RA_SRC, RA_DST)};
            if (FAULT_DROP_WRITE_N > 0) f = {f, $sformatf(" every %0d. write beat dropped;", FAULT_DROP_WRITE_N)};
            if (f != "") $display("sdram_model: FAULT INJECTION ACTIVE:%s", f);
        end
    endtask

    // AUTO REFRESH and LOAD MODE REGISTER need all banks idle, tRP after their precharge (p.13, p.44 note 71)
    task automatic check_idle(input string what, input integer at);
        string busy;
        string early;
        integer n;
        begin
            busy = "";
            early = "";
            for (n = 0; n < 4; n = n + 1) begin
                if (!b_known[n]) busy = {busy, $sformatf(" bank %0d was not precharged since power-up;", n)};
                else if (b_open[n]) busy = {busy, $sformatf(" bank %0d has row %0d open%s;", n, b_row[n],
                                                            pick(b_ap[n], " (auto precharge pending)", ""))};
                else if (at - b_pre[n] < TRP)
                    early = {early, $sformatf(" bank %0d only %0d clock(s) after its %s at edge %0d;", n, at - b_pre[n],
                                              pick(b_pre_ap[n], "auto precharge", "PRECHARGE"), b_pre[n])};
            end
            if (busy != "") report("not_idle", {what, " needs all banks idle:", busy});
            if (early != "") report("tRP", {what, $sformatf(" needs tRP (%0d clocks) after the precharge:", TRP), early});
        end
    endtask

    // READ and WRITE: initialised, the bank open and not auto-precharging, tRCD
    task automatic access_checks(input string what, input integer at);
        begin
            if (!init_ok) report("init_order", {what, " before the initialisation is complete (missing:", init_missing(), ")"});
            if (b_ap[ba] || (b_pre_ap[ba] && !b_open[ba] && at - b_pre[ba] < TRP))
                report("auto_precharge", $sformatf("%s to bank %0d during its auto precharge (READ/WRITE with A10 = 1 at edge %0d; the bank is idle tRP after the precharge starts, p.42 note 60)",
                                                   what, ba, b_ap_cmd[ba]));
            else if (!b_open[ba]) report("bank_closed", $sformatf("%s to bank %0d, which has no open row", what, ba));
            else if (at - b_act[ba] < TRCD)
                report("tRCD", $sformatf("%s to bank %0d only %0d clock(s) after its ACTIVE at edge %0d (tRCD = %0d clocks)",
                                         what, ba, at - b_act[ba], b_act[ba], TRCD));
            if (a[10] && bl == 0)
                report("auto_precharge", {what, " with auto precharge (A10 = 1) in full-page mode, which the chip does not support (p.12)"});
        end
    endtask

    // PRECHARGE or BURST TERMINATE at edge 'at' cuts the read burst: the beats fetched before
    // 'at' come out (CL-1 more), the rest are lost (p.24-26)
    task automatic cut_read(input string by, input integer at);
        begin
            if (rd_bl == 0) rd_on = 1'b0;           // full page: this is how it ends
            else begin
                if (TRUNC_IS_ERROR != 0)
                    report("truncation", $sformatf("%s at edge %0d cuts the READ of edge %0d (bank %0d row %0d column %0d, burst length %0d) short: beats %0d..%0d (due at edges %0d..%0d) are lost",
                                                   by, at, rd_r, rd_bank, rd_row, rd_col, rd_bl, rd_k, rd_bl - 1, at + cl, at + cl + rd_bl - 1 - rd_k));
                rd_ghost = 1'b1;
            end
            own_until = at + cl - 1 + BUS_HOLD;
        end
    endtask

    // a command at edge 'at' ends the write burst: data from 'at' on is not written (p.30-34)
    task automatic cut_write(input string by, input integer at);
        begin
            if (wr_bl != 0 && TRUNC_IS_ERROR != 0)
                report("truncation", $sformatf("%s at edge %0d cuts the WRITE of edge %0d (bank %0d row %0d column %0d, burst length %0d) short: beats %0d..%0d (edges %0d..%0d) are not written",
                                               by, at, wr_w, wr_bank, wr_row, wr_col, wr_bl, wr_k, wr_bl - 1, at, at + wr_bl - 1 - wr_k));
            wr_on = 1'b0;
        end
    endtask

    // LOAD MODE REGISTER (BA = 00, p.6-8 Table 1)
    task automatic decode_mr(input integer at);
        integer ncl;
        integer nbl;
        reg     legal;
        string  s_cl;
        string  s_bl;
        begin
            case (a[6:4])
                3'b010:  ncl = 2;
                3'b011:  ncl = 3;
                default: ncl = 0;
            endcase
            case (a[2:0])
                3'b000:  nbl = 1;
                3'b001:  nbl = 2;
                3'b010:  nbl = 4;
                3'b011:  nbl = 8;
                3'b111:  nbl = 0;
                default: nbl = -1;
            endcase
            if (ncl == 0) s_cl = $sformatf("reserved (A6:A4 = %b)", a[6:4]);
            else s_cl = $sformatf("%0d", ncl);
            if (nbl < 0) s_bl = $sformatf("reserved (A2:A0 = %b)", a[2:0]);
            else if (nbl == 0) s_bl = "full page";
            else s_bl = $sformatf("%0d", nbl);
            $display("sdram_model: clock %0d: LOAD MODE REGISTER A = 0x%04x: CAS latency %s, burst length %s, %s, %s%s",
                     at, a, s_cl, s_bl, pick(a[3], "interleaved", "sequential"),
                     pick(a[9], "single-location writes (A9 = 1)", "burst writes (A9 = 0)"),
                     pick(a[12:10] != 3'b000 || a[8:7] != 2'b00, ", reserved bits set", ""));
            legal = 1'b1;
            if (a[12:10] != 3'b000) begin
                legal = 1'b0;
                report("mode_reg", $sformatf("LOAD MODE REGISTER with A12:A10 = %b; they must be 0", a[12:10]));
            end
            if (a[8:7] != 2'b00) begin
                legal = 1'b0;
                report("mode_reg", $sformatf("LOAD MODE REGISTER with operating mode A8:A7 = %b; only 00 (standard) is defined", a[8:7]));
            end
            if (ncl == 0) begin
                legal = 1'b0;
                report("mode_reg", $sformatf("LOAD MODE REGISTER with CAS latency code A6:A4 = %b; only 010 (CL2) and 011 (CL3) are accepted", a[6:4]));
            end
            if (nbl < 0) begin
                legal = 1'b0;
                report("mode_reg", $sformatf("LOAD MODE REGISTER with reserved burst length code A2:A0 = %b", a[2:0]));
            end
            if (nbl == 0 && a[3]) begin
                legal = 1'b0;
                report("mode_reg", "LOAD MODE REGISTER with a full-page burst and interleaved burst type (A3 = 1), which is not supported");
            end
            if (ncl == 2 && CLK_NS < 12.0)
                report("cl_clock", $sformatf("CAS latency 2 with a %0.3f ns clock: CL2 is specified only up to 83 MHz (tCK >= 12 ns, p.10 Table 3, p.16)", CLK_NS));
            if (ncl == 3 && CLK_NS < 6.0)
                report("cl_clock", $sformatf("CAS latency 3 with a %0.3f ns clock: CL3 is specified only up to 166 MHz (tCK >= 6 ns)", CLK_NS));
            if (ncl != 0) cl = ncl;
            if (nbl >= 0) bl = nbl;
            il = a[3];
            wb_single = a[9];
            mr_ok = legal && at - t_start >= TINIT;
        end
    endtask

    // LOAD EXTENDED MODE REGISTER (BA = 10, p.10-11 Tables 4 and 5)
    task automatic decode_emr(input integer at);
        reg    legal;
        reg    pasr_ok;
        string s_pasr;
        string s_ds;
        begin
            pasr_ok = 1'b1;
            case (a[2:0])
                3'b000:  s_pasr = "all banks";
                3'b001:  s_pasr = "half array (BA1 = 0)";
                3'b010:  s_pasr = "quarter array (BA1 = BA0 = 0)";
                3'b101:  s_pasr = "one eighth";
                3'b110:  s_pasr = "one sixteenth";
                default: begin s_pasr = $sformatf("reserved (A2:A0 = %b)", a[2:0]); pasr_ok = 1'b0; end
            endcase
            case (a[7:5])
                3'b000:  s_ds = "100 %";
                3'b001:  s_ds = "50 %";
                3'b010:  s_ds = "25 %";
                3'b011:  s_ds = "12.5 %";
                3'b100:  s_ds = "75 %";
                default: s_ds = $sformatf("reserved (A7:A5 = %b)", a[7:5]);
            endcase
            $display("sdram_model: clock %0d: LOAD EXTENDED MODE REGISTER A = 0x%04x: partial-array self refresh %s, drive strength %s%s",
                     at, a, s_pasr, s_ds, pick(a[12:8] != 5'd0 || a[4:3] != 2'd0, ", reserved bits set", ""));
            legal = 1'b1;
            if (a[12:8] != 5'd0) begin
                legal = 1'b0;
                report("ext_mode_reg", $sformatf("LOAD EXTENDED MODE REGISTER with A12:A8 = %b; they must be 0", a[12:8]));
            end
            if (a[4:3] != 2'd0) begin
                legal = 1'b0;
                report("ext_mode_reg", $sformatf("LOAD EXTENDED MODE REGISTER with A4:A3 = %b; they must be 0", a[4:3]));
            end
            if (!pasr_ok) begin
                legal = 1'b0;
                report("ext_mode_reg", $sformatf("LOAD EXTENDED MODE REGISTER with reserved PASR code A2:A0 = %b", a[2:0]));
            end
            if (a[7:5] > 3'b100) begin
                legal = 1'b0;
                report("ext_mode_reg", $sformatf("LOAD EXTENDED MODE REGISTER with reserved drive strength code A7:A5 = %b", a[7:5]));
            end
            emr_ok = legal && at - t_start >= TINIT;
        end
    endtask

    always @(posedge clk) begin
        e = cyc;
        if (!started && cke) begin
            started = 1'b1;
            t_start = e;
            startup_message();
        end

        if (started) begin
            if (!cke) begin
                if (!cke_low) report("cke", "CKE low: power-down, self refresh and clock suspend are not modelled; keep CKE high");
                cke_low = 1'b1;
            end else cke_low = 1'b0;

            // the bus in the clock that ends at this edge
            if (dq_i_en && e >= own_from && e <= own_until && !conflict_seen) begin
                report("bus_conflict", $sformatf("dq_i_en is high in the clock ending at this edge, but the chip owns DQ from edge %0d to edge %0d (from the clock after the READ at edge %0d until BUS_HOLD = %0d clocks after its last beat)",
                                                 own_from, own_until, own_read, BUS_HOLD));
                conflict_seen = 1'b1;
            end

            if (!init_seen && dqm != 2'b11 && !dqm_flagged) begin
                report("dqm_init", $sformatf("DQM = %b during the initialisation; both DQM must stay high until it is complete (p.6 Figure 1)", dqm));
                dqm_flagged = 1'b1;
            end

            // auto precharges that start now
            for (i = 0; i < 4; i = i + 1)
                if (b_ap[i] && b_ap_at[i] <= e) begin
                    if (b_ap_at[i] - b_act[i] < TRAS)
                        report("tRAS", $sformatf("the auto precharge of bank %0d (READ/WRITE with A10 = 1 at edge %0d) starts at edge %0d, only %0d clock(s) after its ACTIVE at edge %0d (tRAS = %0d clocks; no tRAS lock-out is documented)",
                                                 i, b_ap_cmd[i], b_ap_at[i], b_ap_at[i] - b_act[i], b_act[i], TRAS));
                    b_ap[i] = 1'b0;
                    b_open[i] = 1'b0;
                    b_pre[i] = b_ap_at[i];
                    b_pre_ap[i] = 1'b1;
                end

            for (i = 0; i < 4; i = i + 1)
                if (b_open[i] && !b_rasmax[i] && e - b_act[i] > TRAS_MAX) begin
                    report("tRAS_max", $sformatf("row %0d of bank %0d has been open since edge %0d, longer than tRAS max (%0d clocks)",
                                                 b_row[i], i, b_act[i], TRAS_MAX));
                    b_rasmax[i] = 1'b1;
                end

            if (t_ref != NEVER) begin
                if (e - t_ref > ref_max) ref_max = e - t_ref;
                if (!ref_late && e - t_ref > TREF_MAX) begin
                    report("refresh_interval", $sformatf("no AUTO REFRESH for %0d clocks since the one at edge %0d (%0.1f ns; the limit is %0d clocks, %0.1f ns x REF_FACTOR %0.2f)",
                                                         e - t_ref, t_ref, (e - t_ref) * CLK_NS, TREF_MAX, T_REFI_NS, REF_FACTOR));
                    ref_late = 1'b1;
                end
            end

            cmd = {ras_n, cas_n, we_n};
            if (cmd != 3'b111) begin
                if (e - t_start < TINIT)
                    report("init_wait", $sformatf("%s %0d clocks (%0.1f ns) after the first edge with CKE high (edge %0d): only NOPs are allowed for the first %0d clocks (%0.0f ns, p.5)",
                                                  cmd_name(cmd), e - t_start, (e - t_start) * CLK_NS, t_start, TINIT, T_INIT_NS));
                if (e - t_ref < TRFC)
                    report("tRFC", $sformatf("%s only %0d clock(s) after the AUTO REFRESH at edge %0d (tRFC = %0d clocks; nothing may interrupt a refresh, p.42 note 61)",
                                             cmd_name(cmd), e - t_ref, t_ref, TRFC));
                if (e - t_mrs < TMRD)
                    report("tMRD", $sformatf("%s only %0d clock(s) after the LOAD MODE REGISTER at edge %0d (tMRD = %0d clocks)",
                                             cmd_name(cmd), e - t_mrs, t_mrs, TMRD));
            end

            case (cmd)
                3'b011: begin                                                   // ACTIVE
                    if (!init_ok) report("init_order", {"ACTIVE before the initialisation is complete (missing:", init_missing(), ")"});
                    if (b_ap[ba])
                        report("auto_precharge", $sformatf("ACTIVE to bank %0d during its READ/WRITE with auto precharge at edge %0d", ba, b_ap_cmd[ba]));
                    else if (b_open[ba])
                        report("bank_open", $sformatf("ACTIVE to bank %0d, which already has row %0d open (ACTIVE at edge %0d)", ba, b_row[ba], b_act[ba]));
                    else if (b_known[ba] && e - b_pre[ba] < TRP)
                        report("tRP", $sformatf("ACTIVE to bank %0d only %0d clock(s) after its %s at edge %0d (tRP = %0d clocks)",
                                                ba, e - b_pre[ba], pick(b_pre_ap[ba], "auto precharge started", "PRECHARGE"), b_pre[ba], TRP));
                    if (e - b_act[ba] < TRC)
                        report("tRC", $sformatf("ACTIVE to bank %0d only %0d clock(s) after its previous ACTIVE at edge %0d (tRC = %0d clocks)",
                                                ba, e - b_act[ba], b_act[ba], TRC));
                    for (i = 0; i < 4; i = i + 1)
                        if (i[1:0] != ba && e - b_act[i] < TRRD)
                            report("tRRD", $sformatf("ACTIVE to bank %0d only %0d clock(s) after the ACTIVE to bank %0d at edge %0d (tRRD = %0d clocks)",
                                                     ba, e - b_act[i], i, b_act[i], TRRD));
                    b_open[ba] = 1'b1;
                    b_row[ba] = a;
                    b_act[ba] = e;
                    b_wr[ba] = NEVER;
                    b_rasmax[ba] = 1'b0;
                end

                3'b101: begin                                                   // READ
                    access_checks("READ", e);
                    // the running bursts end here (p.18-21, p.32); another bank's auto
                    // precharge starts now (READ burst) or tWR from now (WRITE burst)
                    if (rd_on && !rd_ghost) begin
                        if (rd_bl != 0 && TRUNC_IS_ERROR != 0)
                            report("truncation", $sformatf("READ at edge %0d cuts the READ of edge %0d (bank %0d row %0d column %0d, burst length %0d) short: beats %0d..%0d (due at edges %0d..%0d) are lost",
                                                           e, rd_r, rd_bank, rd_row, rd_col, rd_bl, rd_k, rd_bl - 1, e + cl, e + cl + rd_bl - 1 - rd_k));
                        if (rd_ap && b_ap[rd_bank] && b_ap_at[rd_bank] > e) b_ap_at[rd_bank] = e;
                    end
                    if (wr_on) begin
                        if (wr_ap && b_ap[wr_bank] && b_ap_at[wr_bank] > e + TWR) b_ap_at[wr_bank] = e + TWR;
                        cut_write("READ", e);
                    end
                    rd_on = 1'b1;
                    rd_ghost = 1'b0;
                    rd_bank = ba;
                    rd_row = b_row[ba];
                    rd_col = a[9:0];
                    rd_k = 0;
                    rd_bl = bl;
                    rd_r = e;
                    rd_ap = a[10] && bl != 0;
                    if (rd_ap) begin
                        b_ap[ba] = 1'b1;
                        b_ap_at[ba] = e + bl;
                        b_ap_cmd[ba] = e;
                    end
                    last_ap = rd_ap;
                    if (own_until < e + 1) begin
                        own_from = e + 1;
                        conflict_seen = 1'b0;
                    end
                    own_until = bl == 0 ? FOREVER : e + cl + bl - 1 + BUS_HOLD;
                    own_read = e;
                end

                3'b100: begin                                                   // WRITE
                    access_checks("WRITE", e);
                    // the chip stops driving: read beats due after this edge are lost (p.21-23)
                    lost = 0;
                    first = FOREVER;
                    for (j = 3; j >= 1; j = j - 1) begin
                        s = (e + j) & 3;
                        if (p_v[s[1:0]] && p_drv[s[1:0]]) begin
                            p_drv[s[1:0]] = 1'b0;
                            lost = lost + 1;
                            first = e + j;
                        end
                    end
                    if (rd_on && !rd_ghost) begin
                        if (rd_bl == 0) rd_on = 1'b0;
                        else begin
                            lost = lost + rd_bl - rd_k;
                            if (first == FOREVER) first = e + cl;
                            rd_ghost = 1'b1;
                        end
                        if (rd_ap && b_ap[rd_bank] && b_ap_at[rd_bank] > e) b_ap_at[rd_bank] = e;
                    end
                    if (lost > 0 && rd_bl != 0 && TRUNC_IS_ERROR != 0)
                        report("truncation", $sformatf("WRITE at edge %0d cuts read data short: %0d beat(s) from edge %0d on are lost (last READ at edge %0d, bank %0d row %0d column %0d, burst length %0d)",
                                                       e, lost, first, rd_r, rd_bank, rd_row, rd_col, rd_bl));
                    if (own_until > e + BUS_HOLD) own_until = e + BUS_HOLD;
                    if (wr_on) begin
                        if (wr_ap && b_ap[wr_bank] && b_ap_at[wr_bank] > e + TWR) b_ap_at[wr_bank] = e + TWR;
                        cut_write("WRITE", e);
                    end
                    wr_on = 1'b1;
                    wr_bank = ba;
                    wr_row = b_row[ba];
                    wr_col = a[9:0];
                    wr_k = 0;
                    wr_bl = wb_single ? 1 : bl;
                    wr_w = e;
                    wr_ap = a[10] && wr_bl != 0;
                    if (wr_ap) begin
                        b_ap[ba] = 1'b1;
                        b_ap_at[ba] = e + wr_bl - 1 + TWR;
                        b_ap_cmd[ba] = e;
                    end
                    last_ap = wr_ap;
                end

                3'b110: begin                                                   // BURST TERMINATE
                    pend = 1'b0;
                    for (j = 1; j <= 3; j = j + 1) begin
                        s = (e + j) & 3;
                        if (p_v[s[1:0]] && p_drv[s[1:0]]) pend = 1'b1;
                    end
                    if (!(rd_on && !rd_ghost) && !wr_on && !pend)
                        report("burst_terminate", "BURST TERMINATE with no READ or WRITE burst in progress (p.42-43 Table 10)");
                    else if (last_ap && ((rd_on && !rd_ghost) || wr_on))
                        report("burst_terminate", "BURST TERMINATE of a READ/WRITE with auto precharge, which must not be interrupted (p.42 note 60)");
                    if (rd_on && !rd_ghost) cut_read("BURST TERMINATE", e);
                    if (wr_on) cut_write("BURST TERMINATE", e);
                end

                3'b010: begin                                                   // PRECHARGE
                    for (i = 0; i < 4; i = i + 1)
                        if (a[10] || ba == i[1:0]) begin
                            if (b_ap[i])
                                report("auto_precharge", $sformatf("PRECHARGE of bank %0d during its READ/WRITE with auto precharge at edge %0d", i, b_ap_cmd[i]));
                            else if (b_open[i]) begin
                                if (e - b_act[i] < TRAS)
                                    report("tRAS", $sformatf("PRECHARGE of bank %0d only %0d clock(s) after its ACTIVE at edge %0d (tRAS = %0d clocks)",
                                                             i, e - b_act[i], b_act[i], TRAS));
                                if (e - b_wr[i] < TWR)
                                    report("tWR", $sformatf("PRECHARGE of bank %0d only %0d clock(s) after its last write data at edge %0d (tWR = %0d clocks)",
                                                            i, e - b_wr[i], b_wr[i], TWR));
                            end else if (b_known[i] && e - b_pre[i] < TRP) begin
                                if (b_pre_ap[i])
                                    report("auto_precharge", $sformatf("PRECHARGE of bank %0d only %0d clock(s) after its auto precharge started at edge %0d (tRP = %0d clocks, p.42 note 60)",
                                                                       i, e - b_pre[i], b_pre[i], TRP));
                                else
                                    report("precharge_busy", $sformatf("PRECHARGE of bank %0d only %0d clock(s) after its PRECHARGE at edge %0d: no command to a bank while it precharges (tRP = %0d clocks, p.42 notes 60 and 64)",
                                                                       i, e - b_pre[i], b_pre[i], TRP));
                            end
                        end
                    // the bursts of the precharged bank(s) end here
                    if (rd_on && !rd_ghost && (a[10] || ba == rd_bank)) cut_read(pick(a[10], "PRECHARGE ALL", "PRECHARGE"), e);
                    if (wr_on && (a[10] || ba == wr_bank)) begin
                        if (dqm != 2'b11)
                            report("precharge_truncate", $sformatf("PRECHARGE at edge %0d cuts the WRITE of edge %0d (bank %0d) while DQM = %b: the data on the PRECHARGE edge must be masked (p.31)",
                                                                   e, wr_w, wr_bank, dqm));
                        cut_write(pick(a[10], "PRECHARGE ALL", "PRECHARGE"), e);
                    end
                    for (i = 0; i < 4; i = i + 1)
                        if ((a[10] || ba == i[1:0]) && (b_open[i] || b_ap[i] || !b_known[i])) begin
                            b_open[i] = 1'b0;
                            b_ap[i] = 1'b0;
                            b_known[i] = 1'b1;
                            b_pre[i] = e;
                            b_pre_ap[i] = 1'b0;
                        end
                end

                3'b001: begin                                                   // AUTO REFRESH
                    check_idle("AUTO REFRESH", e);
                    t_ref = e;
                    ref_late = 1'b0;
                    ref_cnt = ref_cnt + 1;
                    if (e - t_start >= TINIT) ar_init = ar_init + 1;
                end

                3'b000: begin                                                   // LOAD MODE REGISTER
                    check_idle("LOAD MODE REGISTER", e);
                    if (ba == 2'b00) decode_mr(e);
                    else if (ba == 2'b10) decode_emr(e);
                    else report("mode_reg", $sformatf("LOAD MODE REGISTER with BA = %b, which selects no register (00: mode register, 10: extended mode register)", ba));
                    t_mrs = e;
                end

                default: ;                                                      // NOP
            endcase
            init_ok = ar_init >= 2 && mr_ok && emr_ok;
            if (init_ok) init_seen = 1'b1;

            // write data at this edge (p.30)
            if (wr_on) begin
                col = burst_col(wr_col, wr_k, wr_bl, il);
                if (dqm != 2'b11 && !dq_i_en)
                    report("write_undriven", $sformatf("WRITE beat %0d of edge %0d (bank %0d row %0d column %0d) is not masked (DQM = %b) but dq_i_en is low",
                                                       wr_k, wr_w, wr_bank, wr_row, col, dqm));
                if (dqm != 2'b11) b_wr[wr_bank] = e;
                m = dqm_fault(dqm);
                if (m != 2'b11) begin
                    wr_count = wr_count + 1;
                    if (!(FAULT_DROP_WRITE_N > 0 && wr_count % DROP_N == 0)) begin
                        key = array_key(wr_bank, wr_row, col);
                        old = mem_rd(key);
                        din = dq_fault(dq_i);
                        mem[{7'd0, key}] = {m[1] ? old[15:8] : din[15:8], m[0] ? old[7:0] : din[7:0]};
                    end
                end
                wr_k = wr_k + 1;
                if (wr_bl != 0 && wr_k == wr_bl) wr_on = 1'b0;
            end

            // read: fetch the beat that is valid CL edges from now
            if (rd_on) begin
                col = burst_col(rd_col, rd_k, rd_bl, il);
                s = (e + cl) & 3;
                p_v[s[1:0]] = 1'b1;
                p_drv[s[1:0]] = !rd_ghost;
                p_d[s[1:0]] = mem_rd(array_key(rd_bank, rd_row, col));
                p_addr[s[1:0]] = {rd_bank, rd_row, col};
                p_r[s[1:0]] = rd_r;
                rd_k = rd_k + 1;
                if (rd_bl != 0 && rd_k == rd_bl) rd_on = 1'b0;
            end
        end

        // drive the beat that is valid at the next edge; DQM from the edge before this one
        s = (e + 1) & 3;
        if (p_v[s[1:0]]) begin
            m = dqm_fault(dqm_d1);
            v = dq_fault(p_d[s[1:0]]);
            if (!p_drv[s[1:0]]) begin
                dq_o <= ~p_d[s[1:0]];
                dq_o_en <= 1'b0;
            end else begin
                if (READ_MASK_IS_ERROR != 0 && dqm_d1 != 2'b00)
                    report("read_mask", $sformatf("the read beat valid at edge %0d (READ at edge %0d, bank %0d row %0d column %0d) is masked by DQM = %b at edge %0d: masked byte lanes are not driven",
                                                  e + 1, p_r[s[1:0]], p_addr[s[1:0]][24:23], p_addr[s[1:0]][22:10], p_addr[s[1:0]][9:0], dqm_d1, e - 1));
                dq_o <= {m[1] ? ~p_d[s[1:0]][15:8] : v[15:8], m[0] ? ~p_d[s[1:0]][7:0] : v[7:0]};
                dq_o_en <= m != 2'b11;
            end
            p_v[s[1:0]] = 1'b0;
        end else begin
            dq_o <= 16'h0000;
            dq_o_en <= 1'b0;
        end
        dqm_d1 = dqm;

        error_count <= nerr;
        error <= nerr != 0;
        init_done <= init_ok;
        max_refresh_interval <= ref_max;
        refresh_count <= ref_cnt;
        cyc = cyc + 1;
    end

    /* verilator lint_on BLKSEQ */

endmodule

`ifdef SDRAM_MODEL_SELFTEST
// Top of the model's self-test (sim/sdram_model): one model per parameter set; sel picks the
// one that sees CKE and drives the outputs, the others never start and stay silent.
//   0 defaults (13.468 ns)                1 CLK_NS = 10            2 REF_FACTOR = 16, tRC = 100 ns
//   3 TRUNC/READ_MASK_IS_ERROR = 0        4 DQ4 stuck at 0         5 DQ2 stuck at 1
//   6 DQ0 and DQ8 shorted                 7 LDQM seen low          8 UDQM seen low
//   9 DQML/DQMH swapped                   10 BA1 stuck at 0        11 row bit 0 aliased onto 12
//   12 every 3rd write dropped
module sdram_model_selftest (
    input   wire            clk,
    input   wire    [3:0]   sel,
    input   wire            cke,
    input   wire            ras_n,
    input   wire            cas_n,
    input   wire            we_n,
    input   wire    [1:0]   ba,
    input   wire    [12:0]  a,
    input   wire    [1:0]   dqm,
    input   wire    [15:0]  dq_i,
    input   wire            dq_i_en,
    output  wire    [15:0]  dq_o,
    output  wire            dq_o_en,
    output  wire            error,
    output  wire    [31:0]  error_count,
    output  wire            init_done,
    output  wire    [31:0]  max_refresh_interval,
    output  wire    [31:0]  refresh_count
);
    localparam integer N = 13;
    wire    [15:0]  o_dq    [0:N-1];
    wire            o_en    [0:N-1];
    wire            o_err   [0:N-1];
    wire    [31:0]  o_cnt   [0:N-1];
    wire            o_init  [0:N-1];
    wire    [31:0]  o_rmax  [0:N-1];
    wire    [31:0]  o_rcnt  [0:N-1];

    genvar g;
    generate
        for (g = 0; g < N; g = g + 1) begin : cfg
            localparam [3:0] ID = g;
            sdram_model #(
                .CLK_NS              (g == 1 ? 10.0 : 13.468),
                .REF_FACTOR          (g == 2 ? 16.0 : 1.0),
                .T_RC_NS             (g == 2 ? 100.0 : 60.0),
                .TRUNC_IS_ERROR      (g == 3 ? 0 : 1),
                .READ_MASK_IS_ERROR  (g == 3 ? 0 : 1),
                .FAULT_DQ_STUCK0     (g == 4 ? 16'h0010 : 16'h0000),
                .FAULT_DQ_STUCK1     (g == 5 ? 16'h0004 : 16'h0000),
                .FAULT_DQ_SHORT_A    (g == 6 ? 0 : -1),
                .FAULT_DQ_SHORT_B    (g == 6 ? 8 : -1),
                .FAULT_LDQM_LOW      (g == 7 ? 1 : 0),
                .FAULT_UDQM_LOW      (g == 8 ? 1 : 0),
                .FAULT_DQM_SWAP      (g == 9 ? 1 : 0),
                .FAULT_BA1_STUCK0    (g == 10 ? 1 : 0),
                .FAULT_ROW_ALIAS_SRC (g == 11 ? 0 : -1),
                .FAULT_ROW_ALIAS_DST (g == 11 ? 12 : -1),
                .FAULT_DROP_WRITE_N  (g == 12 ? 3 : 0)
            ) u (
                .clk(clk), .cke(cke && sel == ID), .ras_n(ras_n), .cas_n(cas_n), .we_n(we_n),
                .ba(ba), .a(a), .dqm(dqm), .dq_i(dq_i), .dq_i_en(dq_i_en),
                .dq_o(o_dq[g]), .dq_o_en(o_en[g]), .error(o_err[g]), .error_count(o_cnt[g]),
                .init_done(o_init[g]), .max_refresh_interval(o_rmax[g]), .refresh_count(o_rcnt[g])
            );
        end
    endgenerate

    assign dq_o = o_dq[sel];
    assign dq_o_en = o_en[sel];
    assign error = o_err[sel];
    assign error_count = o_cnt[sel];
    assign init_done = o_init[sel];
    assign max_refresh_interval = o_rmax[sel];
    assign refresh_count = o_rcnt[sel];
endmodule
`endif
