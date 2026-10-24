//
// Behavioural model of one die of the Pocket's PSRAM (AS1C8M16PL-70) in asynchronous
// mode, for simulation only.
//
// The controller's outputs all change on clk, so the model samples them there and
// counts time in clk cycles from the edge that set adv_n low. Reads return 16'hBAD0
// until the 70 ns access time has passed; a write released before 70 ns sets
// timing_error. Only die 0 (ce0_n) is modelled.
//

module psram_model #(
    parameter real  CLK_NS = 13.468,        // clk period
    parameter real  ACCESS_NS = 70.0        // t_AA / t_AW
) (
    input   wire            clk,
    input   wire    [21:16] a,
    inout   wire    [15:0]  dq,
    input   wire            adv_n,
    input   wire            ce0_n,
    input   wire            oe_n,
    input   wire            we_n,
    input   wire            ub_n,
    input   wire            lb_n,
    output  reg             timing_error = 0
);

    reg [15:0]  mem [0:(1<<22)-1];
    reg [21:0]  addr;
    integer     t = 0;                      // clk edges since the address was latched
    reg         we_n_d = 1;
    reg         ub_n_d = 1;
    reg         lb_n_d = 1;
    reg [15:0]  dq_d;

    wire        elapsed = (t + 1) * CLK_NS >= ACCESS_NS;
    wire        drive = !ce0_n && !oe_n && we_n;
    assign dq = drive ? (elapsed ? mem[addr] : 16'hBAD0) : 16'hzzzz;

always @(posedge clk) begin
    we_n_d <= we_n;
    ub_n_d <= ub_n;
    lb_n_d <= lb_n;
    dq_d <= dq;
    t <= t + 1;

    if(!ce0_n && !adv_n) begin
        addr <= {a, dq};
        t <= 0;
    end

    // a write ends when we_n rises (or the chip is deselected while it is low);
    // the data and byte enables are the ones from the edge before
    if(!we_n_d && (we_n || ce0_n)) begin
        if(!elapsed) timing_error <= 1;
        if(!ub_n_d) mem[addr][15:8] <= dq_d[15:8];
        if(!lb_n_d) mem[addr][7:0] <= dq_d[7:0];
    end
end

endmodule
