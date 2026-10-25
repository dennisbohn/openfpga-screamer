//
// 32-bit accesses to one of the Pocket's PSRAMs (AS1C8M16PL-70, two dies of 4M x 16)
// through agg23's asynchronous controller (ip/psram.sv, docs/speicher.md)
//
// A 32-bit access is two halfword accesses, low half first; a write skips a half with no
// mask bits. Byte address [23] selects the die, [22:1] the halfword in it. psram.sv takes
// a request in the clock its read_en/write_en is high and reports busy from the next one.
//

module psram32 #(
    parameter real  CLOCK_SPEED = 74.25         // MHz
) (
    input   wire            clk,

    // one 32-bit access at a time: start it when busy is low
    input   wire            start,
    input   wire            we,
    input   wire    [23:2]  addr,
    input   wire    [31:0]  wdata,
    input   wire    [3:0]   mask,               // write byte lanes; reads return all four
    output  wire            busy,
    output  reg             rvalid = 0,         // one clock, with rdata
    output  reg     [31:0]  rdata,

    output  wire    [21:16] cram_a,
    inout   wire    [15:0]  cram_dq,
    input   wire            cram_wait,
    output  wire            cram_clk,
    output  wire            cram_adv_n,
    output  wire            cram_cre,
    output  wire            cram_ce0_n,
    output  wire            cram_ce1_n,
    output  wire            cram_oe_n,
    output  wire            cram_we_n,
    output  wire            cram_ub_n,
    output  wire            cram_lb_n
);

    localparam  S_IDLE = 2'd0;
    localparam  S_REQ  = 2'd1;                  // request high this clock
    localparam  S_WAIT = 2'd2;                  // until the halfword access is done

    reg  [1:0]  state = S_IDLE;
    reg         a_we;
    reg  [23:2] a_addr;
    reg  [31:0] a_wdata;
    reg  [3:0]  a_mask;
    reg         half;
    reg         p_write = 0, p_read = 0;
    wire        p_busy, p_avail;
    wire [15:0] p_data;

    assign busy = state != S_IDLE;

    wire [1:0]  half_mask = half ? a_mask[3:2] : a_mask[1:0];

psram #(
    .CLOCK_SPEED        ( CLOCK_SPEED )
) ctrl (
    .clk                ( clk ),
    .bank_sel           ( a_addr[23] ),
    .addr               ( {a_addr[22:2], half} ),
    .write_en           ( p_write ),
    .data_in            ( half ? a_wdata[31:16] : a_wdata[15:0] ),
    .write_high_byte    ( half_mask[1] ),
    .write_low_byte     ( half_mask[0] ),
    .read_en            ( p_read ),
    .read_avail         ( p_avail ),
    .data_out           ( p_data ),
    .busy               ( p_busy ),
    .cram_a             ( cram_a ),
    .cram_dq            ( cram_dq ),
    .cram_wait          ( cram_wait ),
    .cram_clk           ( cram_clk ),
    .cram_adv_n         ( cram_adv_n ),
    .cram_cre           ( cram_cre ),
    .cram_ce0_n         ( cram_ce0_n ),
    .cram_ce1_n         ( cram_ce1_n ),
    .cram_oe_n          ( cram_oe_n ),
    .cram_we_n          ( cram_we_n ),
    .cram_ub_n          ( cram_ub_n ),
    .cram_lb_n          ( cram_lb_n )
);

always @(posedge clk) begin
    rvalid <= 0;
    p_write <= 0;
    p_read <= 0;

    case(state)
    S_IDLE: if(start) begin
        a_we <= we;
        a_addr <= addr;
        a_wdata <= wdata;
        a_mask <= mask;
        if(we && mask[1:0] == 2'b00) begin
            half <= 1;
        end else begin
            half <= 0;
        end
        p_write <= we;
        p_read <= !we;
        state <= S_REQ;
    end
    S_REQ: state <= S_WAIT;             // psram.sv takes the request at this edge
    S_WAIT: if(a_we ? !p_busy : p_avail) begin
        if(!a_we) begin
            if(half) rdata[31:16] <= p_data;
            else rdata[15:0] <= p_data;
        end
        if(!half && !(a_we && a_mask[3:2] == 2'b00)) begin
            half <= 1;
            p_write <= a_we;
            p_read <= !a_we;
            state <= S_REQ;
        end else begin
            rvalid <= !a_we;
            state <= S_IDLE;
        end
    end
    default: state <= S_IDLE;
    endcase
end

endmodule
