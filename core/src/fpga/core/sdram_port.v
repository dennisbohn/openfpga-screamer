//
// The SDRAM's front end: a command register per CPU port, the bridge write buffer, the bridge
// read register and the arbiter in front of sdram.v (docs/memory.md, "Ports, arbiter and
// ordering"). soc.v instantiates it; the random test (sim/sdram) drives it directly.
//
// Ports
//   iBus and dBus speak VexRiscv's native cache-bus protocol, limited to what soc.v sends
//   here: iBus lines, dBus line reads, single reads and single writes.
//   The line port (the rs_* signals, named after the rasterizer) reads lines the same way as
//   the iBus. soc.v shares it among the mixer, the rasterizer, the geometry unit and the
//   blitter. Up to two lines are in the controller at once: the next one starts while the
//   words of the one before still come in. The words return in the order of the lines;
//   soc.v keeps track of whose they are.
//   cmd_ready is "register empty" and comes from flops only, so the ports may take a
//   command in the same clock.
//
// Arbiter
//   At every job boundary the arbiter picks, in this order: bridge buffer, bridge read, dBus
//   register, line port register, iBus register.
//   A bridge read (the APF saving a nonvolatile slot) is one word. It has the whole next
//   bridge word, 88 clocks at the APF's fastest, until its answer is sampled.
//
// Ordering
//   Writes of one source run in the order they were taken. A read sees every write taken
//   before it (an iBus read may see later ones too).
//
// CPU reset
//   The CPU registers are emptied. Words of reads already in the controller carry the epoch
//   they were issued in (a bit of job_tag, toggled on every CPU reset) and are dropped if the
//   epoch has changed since, however short the reset was. The bridge path, the buffer and the
//   controller go on.
//
// Job tag
//   job_tag = {epoch, port}. The port is P_IB, P_DB, P_BR (the bridge, also its reads) or P_RS
//   (the line port). The controller returns the tag with every word of a read.
//

module sdram_port (
    input   wire            clk,
    input   wire            cpu_reset,          // synchronous, high while the CPU is held

    // iBus lines in the SDRAM: a command takes a line (32 bytes), the response is its 8 words
    // in order, each a clock after the controller delivers it
    input   wire            ib_cmd_valid,
    output  wire            ib_cmd_ready,
    input   wire    [25:5]  ib_cmd_addr,
    output  reg             ib_rsp_valid = 0,
    output  reg     [31:0]  ib_rsp_data,
    output  reg             ib_rsp_error,       // never on the 8th word (VexRiscv I$ hadError)

    // dBus commands for the SDRAM: writes (with a byte mask, no response), single word reads
    // (1 response word) and line reads (8 words)
    input   wire            db_cmd_valid,
    output  wire            db_cmd_ready,
    input   wire            db_cmd_wr,
    input   wire            db_cmd_line,        // read 8 words
    input   wire    [25:2]  db_cmd_addr,
    input   wire    [31:0]  db_cmd_data,
    input   wire    [3:0]   db_cmd_mask,
    output  reg             db_rsp_valid = 0,
    output  reg     [31:0]  db_rsp_data,
    output  reg             db_rsp_error,
    output  reg             db_rsp_last,

    // The line port: lines of 8 words, in the order they were asked for (soc.v shares it, see
    // the header). rs_cmd_ready is low while a command waits or two lines are in the controller.
    input   wire            rs_cmd_valid,
    output  wire            rs_cmd_ready,
    input   wire    [25:5]  rs_cmd_addr,
    output  reg             rs_rsp_valid = 0,
    output  reg     [31:0]  rs_rsp_data,
    output  reg             rs_rsp_last = 0,    // the line's last word

    // bridge writes into the SDRAM, data already in the CPU's byte order
    input   wire            br_wr,
    input   wire    [25:2]  br_addr,
    input   wire    [31:0]  br_data,

    // status and bridge reads
    output  reg     [23:0]  br_words = 0,       // bridge words since power-up or Reset Enter
    output  reg             br_overflow = 0,    // a bridge word found the buffer full
    output  reg             br_dropped = 0,     // a bridge word met an SDRAM not ready
    input   wire            br_rd,              // one clock: read the word at br_rd_addr
    input   wire    [25:2]  br_rd_addr,
    output  reg             br_rd_valid = 0,    // one clock: its word in br_rd_data
    output  reg     [31:0]  br_rd_data,
    output  wire            cpu_writes_idle,    // no CPU write left here or in the controller

    // The controller's job and read ports (sdram.v)
    output  wire            job_valid,
    input   wire            job_ready,
    output  wire            job_write,
    output  wire            job_line,
    output  wire    [25:2]  job_addr,
    output  wire    [31:0]  job_wdata,
    output  wire    [3:0]   job_mask,
    output  wire    [2:0]   job_tag,            // {epoch, port}
    input   wire            rd_valid,
    input   wire    [31:0]  rd_data,
    input   wire    [2:0]   rd_tag,
    input   wire            rd_last,
    input   wire            rd_error,
    input   wire            sdram_ready,
    input   wire            sdram_writing
);

    // ports, as in the job tag
    localparam  [1:0]   P_IB = 2'd0;
    localparam  [1:0]   P_DB = 2'd1;
    localparam  [1:0]   P_BR = 2'd2;
    localparam  [1:0]   P_RS = 2'd3;

    reg         reset_d = 0;
    reg         epoch = 0;                  // toggles on every rise of cpu_reset
    wire        reset_rise = cpu_reset && !reset_d;

//
// The CPU command registers: one entry per port, loaded on acceptance
//
    reg         ib_valid = 0;               // a line waits for the arbiter
    reg         ib_busy = 0;                // a line is in the controller, words to come
    reg  [25:5] ib_addr;

    reg         db_valid = 0;
    reg         db_busy = 0;                // a read is in the controller
    reg         db_wr;
    reg         db_line;
    reg  [25:2] db_addr;
    reg  [31:0] db_data;
    reg  [3:0]  db_mask;

    reg         rs_valid = 0;               // a line request waits for the arbiter
    reg  [1:0]  rs_out = 0;                 // its lines in the controller, up to two
    reg  [25:5] rs_addr;

    assign ib_cmd_ready = !ib_valid && !ib_busy && !cpu_reset;
    assign db_cmd_ready = !db_valid && !db_busy && !cpu_reset;
    assign rs_cmd_ready = !rs_valid && rs_out != 2'd2 && !cpu_reset;

//
// Bridge write buffer, 4 entries. The Pocket delivers at most one word every 88 clocks; a
// word that finds the buffer full sets br_overflow and is lost.
//
    reg  [25:2] bf_addr [0:3];
    reg  [31:0] bf_data [0:3];
    reg  [1:0]  bf_wp = 0, bf_rp = 0;
    reg  [2:0]  bf_count = 0;
    reg         brd_valid = 0;              // a bridge read waits for the arbiter
    reg  [25:2] brd_addr;

//
// Arbiter. The sel_* signals choose the source of the next job, in order of priority.
//
    wire        sel_br = bf_count != 0;
    wire        sel_brd = !sel_br && brd_valid;
    wire        sel_db = !sel_br && !brd_valid && db_valid;
    wire        sel_rs = !sel_br && !brd_valid && !db_valid && rs_valid;
    wire        sel_ib = !sel_br && !brd_valid && !db_valid && !rs_valid && ib_valid;

    assign job_valid = sel_br || sel_brd || sel_db || sel_rs || sel_ib;
    assign job_write = sel_br || (sel_db && db_wr);
    assign job_line  = sel_db ? db_line : sel_rs || sel_ib;
    // job_addr is a word address [25:2]. A line (32 bytes) starts at the word
    // {line address, 3'b000}.
    assign job_addr  = sel_br ? bf_addr[bf_rp] : sel_brd ? brd_addr : sel_db ? db_addr :
                       sel_rs ? {rs_addr, 3'b000} : {ib_addr, 3'b000};
    assign job_wdata = sel_br ? bf_data[bf_rp] : db_data;
    assign job_mask  = sel_br ? 4'b1111 : db_mask;
    assign job_tag   = {epoch, sel_br || sel_brd ? P_BR : sel_db ? P_DB : sel_rs ? P_RS : P_IB};

    wire        job_take = job_valid && job_ready;

    // no CPU write waits in the dBus register or runs in the controller
    assign cpu_writes_idle = !(db_valid && db_wr) && !sdram_writing;

    // Read words for the current epoch. Words of an older epoch belong to a CPU that was reset
    // and are dropped.
    wire        rd_ib = rd_valid && rd_tag == {epoch, P_IB} && !cpu_reset;
    wire        rd_db = rd_valid && rd_tag == {epoch, P_DB} && !cpu_reset;
    wire        rd_rs = rd_valid && rd_tag == {epoch, P_RS} && !cpu_reset;
    wire        rd_br = rd_valid && rd_tag[1:0] == P_BR;    // whatever the epoch: not the CPU's

    // Registers of the front end: command registers, the jobs given to the controller, the
    // words coming back, the bridge read and the bridge buffer.
always @(posedge clk) begin
    reset_d <= cpu_reset;
    if(reset_rise) epoch <= ~epoch;

    // CPU commands in
    if(ib_cmd_valid && ib_cmd_ready) begin
        ib_valid <= 1;
        ib_addr <= ib_cmd_addr;
    end
    if(db_cmd_valid && db_cmd_ready) begin
        db_valid <= 1;
        db_wr <= db_cmd_wr;
        db_line <= db_cmd_line;
        db_addr <= db_cmd_addr;
        db_data <= db_cmd_data;
        db_mask <= db_cmd_mask;
    end

    // Jobs to the controller: the register is emptied when its job is taken. A read keeps
    // its port busy until its last word is back.
    if(job_take && sel_ib) begin
        ib_valid <= 0;
        ib_busy <= 1;
    end
    if(job_take && sel_db) begin
        db_valid <= 0;
        db_busy <= !db_wr;
    end
    if(rs_cmd_valid && rs_cmd_ready) begin
        rs_valid <= 1;
        rs_addr <= rs_cmd_addr;
    end
    if(job_take && sel_rs) rs_valid <= 0;
    rs_out <= rs_out + (job_take && sel_rs ? 2'd1 : 2'd0) - (rd_rs && rd_last ? 2'd1 : 2'd0);

    // Words back, one clock after the controller delivers them.
    ib_rsp_valid <= rd_ib;
    db_rsp_valid <= rd_db;
    rs_rsp_valid <= rd_rs;
    rs_rsp_last <= rd_rs && rd_last;
    if(rd_rs) rs_rsp_data <= rd_data;
    if(rd_ib) begin
        ib_rsp_data <= rd_data;
        ib_rsp_error <= rd_error && !rd_last;
        if(rd_last) ib_busy <= 0;
    end
    if(rd_db) begin
        db_rsp_data <= rd_data;
        db_rsp_error <= rd_error;
        db_rsp_last <= rd_last;
        if(rd_last) db_busy <= 0;
    end

    // CPU reset: nothing waits for the old CPU any more.
    if(cpu_reset) begin
        ib_valid <= 0;
        ib_busy <= 0;
        db_valid <= 0;
        db_busy <= 0;
        rs_valid <= 0;
        rs_out <= 0;
    end

    // Bridge read: one word, answered when its job returns.
    if(br_rd) begin
        brd_valid <= 1;
        brd_addr <= br_rd_addr;
    end else if(job_take && sel_brd) brd_valid <= 0;
    br_rd_valid <= rd_br;
    if(rd_br) br_rd_data <= rd_data;

    // Bridge buffer: a word is dropped (br_dropped) when it reaches an SDRAM that is not
    // ready. A word that finds the buffer full is lost (br_overflow), unless an entry is
    // taken in the same clock.
    if(job_take && sel_br) begin
        bf_rp <= bf_rp + 1'b1;
        if(!sdram_ready) br_dropped <= 1;
    end
    if(br_wr) begin
        br_words <= br_words + 1'b1;
        if(bf_count == 3'd4 && !(job_take && sel_br)) begin
            br_overflow <= 1;
        end else begin
            bf_addr[bf_wp] <= br_addr;
            bf_data[bf_wp] <= br_data;
            bf_wp <= bf_wp + 1'b1;
        end
    end
    bf_count <= bf_count + (br_wr && !(bf_count == 3'd4 && !(job_take && sel_br)) ? 3'd1 : 3'd0)
                         - (job_take && sel_br ? 3'd1 : 3'd0);

    // Reset Enter (the rise of cpu_reset): the counters start over for the slots loaded now.
    if(reset_rise) begin
        br_words <= br_wr ? 24'd1 : 24'd0;
        br_overflow <= 0;
        br_dropped <= 0;
    end
end

endmodule
