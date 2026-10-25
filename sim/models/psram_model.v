//
// Behavioural model of one of the Pocket's PSRAMs (AS1C8M16PL-70: two dies of 4M x 16,
// selected by ce0_n and ce1_n) in asynchronous mode, for simulation only.
//
// The controller's outputs all change on clk, so the model samples them there and
// counts time in clk cycles from the edge that set adv_n low. Reads return 16'hBAD0
// until the 70 ns access time has passed; a write released before 70 ns is an error.
// Also errors: both chip enables low, and any access to halfword 3FFFFFh of a die, which
// the memory test must leave alone (two reads and a write there are the "software
// access" sequence that rewrites the configuration registers). Memory starts with a
// poison pattern. DIE1_DEAD is a fault for negative tests: die 1 never answers.
//

`ifndef PSRAM_FAULT_DIE1_DEAD
`define PSRAM_FAULT_DIE1_DEAD 0     // fault for negative tests, settable as a build define
`endif

module psram_model #(
    parameter real  CLK_NS = 13.468,        // clk period
    parameter real  ACCESS_NS = 70.0,       // t_AA / t_AW
    parameter       DIE1_DEAD = `PSRAM_FAULT_DIE1_DEAD
) (
    input   wire            clk,
    input   wire    [21:16] a,
    inout   wire    [15:0]  dq,
    input   wire            adv_n,
    input   wire            ce0_n,
    input   wire            ce1_n,
    input   wire            oe_n,
    input   wire            we_n,
    input   wire            ub_n,
    input   wire            lb_n,
    output  reg             timing_error = 0,
    output  reg             error = 0,
    output  reg     [31:0]  error_count = 0
);

    reg [15:0]  mem0 [0:(1<<22)-1];
    reg [15:0]  mem1 [0:(1<<22)-1];
    integer     i;
    initial begin
        for(i = 0; i < (1<<22); i = i + 1) begin
            mem0[i] = 16'hC000 ^ i[15:0];
            mem1[i] = 16'hD000 ^ i[15:0];
        end
    end

    reg [21:0]  addr;
    reg         die;                        // the die of the current access
    integer     t = 0;                      // clk edges since the address was latched
    integer     cycle = 0;
    reg         we_n_d = 1;
    reg         ub_n_d = 1;
    reg         lb_n_d = 1;
    reg         ce_d = 0;
    reg [15:0]  dq_d;

    wire        ce0 = !ce0_n;
    wire        ce1 = !ce1_n && !DIE1_DEAD;
    wire        selected = die ? ce1 : ce0;
    wire        elapsed = (t + 1) * CLK_NS >= ACCESS_NS;
    wire        drive = selected && !oe_n && we_n;
    wire [15:0] word = die ? mem1[addr] : mem0[addr];
    assign dq = drive ? (elapsed ? word : 16'hBAD0) : 16'hzzzz;

    task fail(input [8*64-1:0] msg);
        begin
            error <= 1;
            error_count <= error_count + 1;
            if(error_count < 50) $display("psram_model: clock %0d: %0s", cycle, msg);
        end
    endtask

always @(posedge clk) begin
    cycle <= cycle + 1;
    we_n_d <= we_n;
    ub_n_d <= ub_n;
    lb_n_d <= lb_n;
    ce_d <= selected;
    dq_d <= dq;
    t <= t + 1;

    if(!ce0_n && !ce1_n) fail("CE0 and CE1 low together");

    if((ce0 || ce1) && !adv_n) begin
        addr <= {a, dq};
        die <= ce1;
        t <= 0;
        if({a, dq} == 22'h3FFFFF) fail("access to halfword 3FFFFFh (software access word)");
    end

    // a write ends when we_n rises (or the chip is deselected while it is low);
    // the data and byte enables are the ones from the edge before
    if(!we_n_d && ce_d && (we_n || !selected)) begin
        if(!elapsed) begin
            timing_error <= 1;
            fail("write released before 70 ns");
        end
        if(die) begin
            if(!ub_n_d) mem1[addr][15:8] <= dq_d[15:8];
            if(!lb_n_d) mem1[addr][7:0] <= dq_d[7:0];
        end else begin
            if(!ub_n_d) mem0[addr][15:8] <= dq_d[15:8];
            if(!lb_n_d) mem0[addr][7:0] <= dq_d[7:0];
        end
    end
end

endmodule
