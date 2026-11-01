// The SoC simulation does not run the FM chip (sim/opl does): this stands in for jtopl2 with the
// same ports and no output.
module jtopl2 (
    input                  rst,
    input                  clk,
    input                  cen,
    input           [ 7:0] din,
    input                  addr,
    input                  cs_n,
    input                  wr_n,
    output          [ 7:0] dout,
    output                 irq_n,
    output  signed  [15:0] snd,
    output                 sample
);
    assign dout = 8'h00;
    assign irq_n = 1'b1;
    assign snd = 16'sd0;
    assign sample = 1'b0;
endmodule
