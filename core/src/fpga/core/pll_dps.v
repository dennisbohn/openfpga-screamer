//
// One dynamic phase shift step of a PLL output counter at a time (Cyclone V altera_pll,
// pll_subtype "DPS"; docs/speicher.md)
//
// A step moves the phase by one eighth of a VCO period (168 ps for pll_sdram's 742.5 MHz
// VCO). cntsel and updn are set first, phase_en is held for 4 scan clocks, then the PLL
// pulls phase_done low until the step is done. count follows the steps made since the
// bitstream was loaded (signed, positive = later).
//

module pll_dps #(
    parameter [4:0] COUNTER = 5'd0              // C0
) (
    input   wire            clk,                // also the PLL's scanclk
    input   wire            step,               // one clock: make a step
    input   wire            later,              // its direction
    output  wire            busy,
    output  reg     [15:0]  count = 0,

    output  reg     [4:0]   cntsel = 0,
    output  reg             updn = 0,
    output  reg             phase_en = 0,
    input   wire            phase_done
);

    localparam  S_IDLE = 3'd0;
    localparam  S_SET  = 3'd1;                  // cntsel and updn settle
    localparam  S_EN   = 3'd2;                  // phase_en high
    localparam  S_LOW  = 3'd3;                  // until phase_done goes low
    localparam  S_HIGH = 3'd4;                  // until it is high again

    reg  [2:0]  state = S_IDLE;
    reg  [9:0]  cnt;
    reg  [1:0]  done_s = 2'b11;

    assign busy = state != S_IDLE;

always @(posedge clk) begin
    done_s <= {done_s[0], phase_done};
    cnt <= cnt + 1'b1;

    case(state)
    S_IDLE: if(step) begin
        cntsel <= COUNTER;
        updn <= later;
        cnt <= 0;
        state <= S_SET;
    end
    S_SET: if(cnt == 10'd3) begin
        phase_en <= 1;
        cnt <= 0;
        state <= S_EN;
    end
    S_EN: if(cnt == 10'd3) begin
        phase_en <= 0;
        cnt <= 0;
        state <= S_LOW;
    end
    S_LOW: if(!done_s[1] || cnt == 10'd63) begin    // a step that never pulls it low ends anyway
        cnt <= 0;
        state <= S_HIGH;
    end
    S_HIGH: if(done_s[1] || cnt == 10'd1023) begin
        count <= updn ? count + 1'b1 : count - 1'b1;
        state <= S_IDLE;
    end
    default: state <= S_IDLE;
    endcase
end

endmodule
