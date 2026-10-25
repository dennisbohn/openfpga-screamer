//
// Behavioural model of the Pocket's SRAM (AS6C2016-55, 128K x 16, asynchronous) for
// simulation only, with the datasheet's timing checked in clock cycles
//
// The controller's pin registers all change on clk, so the model samples them at posedge
// clk and knows each change happened at the edge before. Times are counted from those
// edges and reduced by PAD_NS for the pins on the way (the controller's IO registers,
// the board and the SRAM's input buffers). The data bus is split: dq_i/dq_i_en is what
// the controller drives, dq_o/dq_o_en what the SRAM drives; the testbench joins them.
//
// Read: the SRAM drives while OE and WE allow it; the data is valid for a sample at edge
// k + 1 only if tAA, tOE and tBA have passed by then (else a poison value). It keeps the
// bus HOLD clocks after OE rises (tOHZ 20 ns); the controller driving in that window is a
// bus conflict. Write: WE-controlled, data taken when WE rises; checked are tWP, tAW,
// tBW, tDW, a stable address while WE is low and data held for a clock after WE rises.
//

`ifndef SRAM_FAULT_SWAP_UB_LB
`define SRAM_FAULT_SWAP_UB_LB 0      // fault for negative tests, settable as a build define
`endif

module sram_model #(
    parameter real  CLK_NS = 13.468,
    parameter real  PAD_NS = 6.0,           // pin and board delay taken off every interval
    parameter       HOLD = 3,               // clocks after OE rises in which the SRAM may still drive
    parameter       SWAP_UB_LB = `SRAM_FAULT_SWAP_UB_LB    // fault: UB and LB swapped
) (
    input   wire            clk,
    input   wire    [16:0]  a,
    input   wire    [15:0]  dq_i,
    input   wire            dq_i_en,
    output  reg     [15:0]  dq_o = 16'h0000,
    output  reg             dq_o_en = 0,
    input   wire            oe_n,
    input   wire            we_n,
    input   wire            ub_n_pin,
    input   wire            lb_n_pin,
    output  reg             error = 0,
    output  reg     [31:0]  error_count = 0
);

    localparam real T_AA = 55.0, T_OE = 30.0, T_BA = 55.0;
    localparam real T_WP = 45.0, T_AW = 50.0, T_BW = 50.0, T_DW = 25.0;

    wire        ub_n = SWAP_UB_LB ? lb_n_pin : ub_n_pin;
    wire        lb_n = SWAP_UB_LB ? ub_n_pin : lb_n_pin;

    reg [15:0]  mem [0:(1<<17)-1];
    integer     i;
    initial begin
        for(i = 0; i < (1<<17); i = i + 1) mem[i] = 16'h5A00 ^ i[15:0];  // poison
    end

    integer     cycle = 0;
    reg [16:0]  a_d = 0;
    reg         oe_n_d = 1, we_n_d = 1, ub_n_d = 1, lb_n_d = 1;
    reg [15:0]  dq_d;
    reg         dq_en_d = 0;
    integer     t_addr = 0, t_oe = 0, t_ub = 0, t_lb = 0, t_we = 0, t_data = 0, t_oe_up = -100;
    integer     t_ub_wr = 0, t_lb_wr = 0;   // byte lane low during the write
    reg         wr_pending = 0;             // WE rose last clock: check the data hold
    reg [15:0]  wr_data;

    task fail(input [8*64-1:0] msg);
        begin
            error <= 1;
            error_count <= error_count + 1;
            if(error_count < 50) $display("sram_model: clock %0d: %0s", cycle, msg);
        end
    endtask

    function real since(input integer t);   // ns from the edge at t to the next sample
        since = (cycle + 1 - t) * CLK_NS - PAD_NS;
    endfunction

    wire        reading = !oe_n && we_n && (!ub_n || !lb_n);

always @(posedge clk) begin
    cycle <= cycle + 1;
    a_d <= a;
    oe_n_d <= oe_n;
    we_n_d <= we_n;
    ub_n_d <= ub_n;
    lb_n_d <= lb_n;
    dq_d <= dq_i;
    dq_en_d <= dq_i_en;

    // changes seen now happened at the previous edge
    if(a != a_d) begin
        t_addr <= cycle - 1;
        if(!we_n) fail("address changed while WE is low");
    end
    if(!oe_n && oe_n_d) t_oe <= cycle - 1;
    if(oe_n && !oe_n_d) t_oe_up <= cycle - 1;
    if(!ub_n && ub_n_d) t_ub <= cycle - 1;
    if(!lb_n && lb_n_d) t_lb <= cycle - 1;
    if(!we_n && we_n_d) t_we <= cycle - 1;
    if(dq_i_en && (!dq_en_d || dq_i != dq_d)) t_data <= cycle - 1;
    if(!we_n && !ub_n && (we_n_d || ub_n_d)) t_ub_wr <= cycle - 1;
    if(!we_n && !lb_n && (we_n_d || lb_n_d)) t_lb_wr <= cycle - 1;
    if(!oe_n && !we_n) fail("OE and WE low together");

    // read: drive the word, valid only once every access time has passed
    if(reading) begin
        dq_o_en <= 1;
        if(since(t_addr) >= T_AA && since(t_oe) >= T_OE &&
           (ub_n || since(t_ub) >= T_BA) && (lb_n || since(t_lb) >= T_BA))
            dq_o <= {ub_n ? 8'hBD : mem[a][15:8], lb_n ? 8'hBD : mem[a][7:0]};
        else
            dq_o <= 16'hBAD5;
    end else if(cycle - t_oe_up >= HOLD) begin
        dq_o_en <= 0;
    end
    if(dq_i_en && (reading || cycle - t_oe_up < HOLD)) fail("bus conflict: controller drives while the SRAM may");

    // write: WE rises (the clock before it was low); check and store
    if(we_n && !we_n_d) begin
        if((cycle - 1 - t_we) * CLK_NS - 2.0 < T_WP) fail("tWP: WE low too short");
        if((cycle - 1 - t_addr) * CLK_NS - 2.0 < T_AW) fail("tAW: address too late before WE rises");
        if(!dq_en_d) fail("write without data driven");
        else if((cycle - 1 - t_data) * CLK_NS - 2.0 < T_DW) fail("tDW: data too late before WE rises");
        if(ub_n_d && lb_n_d) fail("write with neither byte lane");
        if(!ub_n_d && (cycle - 1 - t_ub_wr) * CLK_NS - 2.0 < T_BW) fail("tBW: UB low too short");
        if(!lb_n_d && (cycle - 1 - t_lb_wr) * CLK_NS - 2.0 < T_BW) fail("tBW: LB low too short");
        if(!ub_n_d) mem[a_d][15:8] <= dq_d[15:8];
        if(!lb_n_d) mem[a_d][7:0] <= dq_d[7:0];
        wr_pending <= 1;
        wr_data <= dq_d;
    end else begin
        wr_pending <= 0;
    end
    if(wr_pending && (!dq_en_d || dq_d != wr_data)) fail("tDH: data not held for a clock after WE rises");
end

endmodule
