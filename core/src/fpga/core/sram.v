//
// The Pocket's SRAM (AS6C2016-55, 128K x 16, asynchronous, 55 ns) for 32-bit accesses
// (docs/speicher.md)
//
// A 32-bit access is two halfword accesses, low half first; a write skips a half with no
// mask bits. All pin signals come straight from registers, so Quartus can put them into
// the IO cells, and so does the first register on the data inputs. At 74.25 MHz:
//
//   read   clock 0  address, OE and UB/LB low
//          clock 6  the IO register takes the data (80.8 ns after the address), OE high
//   write  clock 0  address and data, WE high
//          clock 1  WE and the byte lanes of the mask low
//          clock 5  WE and UB/LB high (WE low 53.9 ns >= 45 ns); clock 6 bus released
//
// The address only changes while WE is high, and a turnaround clock separates a read from
// a following write and a write from a following read (the SRAM's drivers turn off 20 ns
// after OE or need 5 ns to turn on).
//

module sram (
    input   wire            clk,

    // one 32-bit access at a time: start it when busy is low
    input   wire            start,
    input   wire            we,
    input   wire    [17:2]  addr,
    input   wire    [31:0]  wdata,
    input   wire    [3:0]   mask,           // write byte lanes; reads return all four
    output  wire            busy,
    output  reg             rvalid = 0,     // one clock, with rdata
    output  reg     [31:0]  rdata,

    output  reg     [16:0]  sram_a,
    output  reg     [15:0]  sram_dq_o,
    output  reg             sram_dq_oe = 0,
    input   wire    [15:0]  sram_dq_i,
    output  reg             sram_oe_n = 1,
    output  reg             sram_we_n = 1,
    output  reg             sram_ub_n = 1,
    output  reg             sram_lb_n = 1
);

    localparam  READ_CLOCKS  = 3'd6;        // address to data in the IO register
    localparam  WRITE_CLOCKS = 3'd6;        // address to bus released

    reg         active = 0;                 // a 32-bit access is running
    reg         a_we;
    reg  [17:2] a_addr;
    reg  [31:0] a_wdata;
    reg  [3:0]  a_mask;
    reg         half;                       // the halfword being worked on
    reg         phase = 0;                  // a halfword access is on the pins
    reg  [2:0]  cnt;
    reg         last_we = 0;                // what the previous halfword access was
    reg         gap = 0;                    // a turnaround clock is due
    reg  [15:0] dq_in;                      // the IO input register

    assign busy = active;

    wire [1:0]  half_mask = half ? a_mask[3:2] : a_mask[1:0];
    wire        half_skip = a_we && half_mask == 2'b00;

always @(posedge clk) begin
    dq_in <= sram_dq_i;
    rvalid <= 0;

    if(!active) begin
        if(start) begin
            active <= 1;
            a_we <= we;
            a_addr <= addr;
            a_wdata <= wdata;
            a_mask <= mask;
            half <= 0;
            gap <= last_we != we;
        end
    end else if(!phase) begin
        // between halfword accesses: wait out a turnaround, skip empty write halves
        if(gap) begin
            gap <= 0;
        end else if(half_skip) begin
            if(half) active <= 0;
            half <= 1;
        end else begin
            phase <= 1;
            cnt <= 0;
            last_we <= a_we;
            sram_a <= {a_addr, half};
            if(a_we) begin
                sram_dq_o <= half ? a_wdata[31:16] : a_wdata[15:0];
                sram_dq_oe <= 1;
            end else begin
                sram_oe_n <= 0;
                sram_ub_n <= 0;
                sram_lb_n <= 0;
            end
        end
    end else begin
        cnt <= cnt + 1'b1;
        if(a_we) begin
            if(cnt == 3'd0) begin
                sram_we_n <= 0;
                sram_ub_n <= !half_mask[1];
                sram_lb_n <= !half_mask[0];
            end
            if(cnt == WRITE_CLOCKS - 3'd2) begin
                sram_we_n <= 1;
                sram_ub_n <= 1;
                sram_lb_n <= 1;
            end
            if(cnt == WRITE_CLOCKS - 3'd1) begin
                sram_dq_oe <= 0;
                phase <= 0;
                half <= 1;
                if(half) active <= 0;
            end
        end else begin
            if(cnt == READ_CLOCKS - 3'd1) begin
                // the IO register takes the data at this edge
                sram_oe_n <= 1;
                sram_ub_n <= 1;
                sram_lb_n <= 1;
            end
            if(cnt == READ_CLOCKS) begin
                if(half) rdata[31:16] <= dq_in;
                else rdata[15:0] <= dq_in;
                phase <= 0;
                half <= 1;
                if(half) begin
                    active <= 0;
                    rvalid <= 1;
                end
            end
        end
    end
end

endmodule
