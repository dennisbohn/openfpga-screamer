//
// 320x200 8-bit framebuffer with a 256-colour palette, shown at 60 Hz
//
// Both live in block RAM for now and are filled through the bridge from the
// data slot file screen.bin (see tools/make_screen.py), loaded to address 0:
//   0x0000-0x03FF  palette, 256 words of R,G,B,0
//   0x0400-0xFDFF  pixels, 320x200 palette indices, 4 per word, leftmost in [31:24]
//
// Video timing at the 12.288 MHz pixel clock: 400 clocks x 512 lines = 60 Hz,
// 320 x 200 of it visible.
//

module fb_video (
    // bridge writes, synchronous to clk_74a
    input   wire            clk_74a,
    input   wire            bridge_wr,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,

    // video out, synchronous to clk_vid
    input   wire            clk_vid,
    input   wire            reset_n,
    output  reg     [23:0]  vid_rgb,
    output  reg             vid_de,
    output  reg             vid_hs,
    output  reg             vid_vs
);

    localparam  H_ACTIVE = 'd320;
    localparam  H_BPORCH = 'd10;
    localparam  H_TOTAL  = 'd400;
    localparam  V_ACTIVE = 'd200;
    localparam  V_BPORCH = 'd10;
    localparam  V_TOTAL  = 'd512;

    localparam  FB_WORDS = H_ACTIVE * V_ACTIVE / 4;

    reg [31:0]  fb [0:FB_WORDS-1];
    reg [23:0]  pal [0:255];

//
// write side
//
    wire        in_window = bridge_addr[31:16] == 16'h0000;
    wire [7:0]  pal_waddr = bridge_addr[9:2];
    wire [13:0] fb_waddr = bridge_addr[15:2] - 14'd256;

always @(posedge clk_74a) begin
    if(bridge_wr && in_window) begin
        if(bridge_addr[15:10] == 6'd0)
            pal[pal_waddr] <= bridge_wr_data[31:8];
        else if(fb_waddr < FB_WORDS)
            fb[fb_waddr] <= bridge_wr_data;
    end
end

//
// read side: counters, then a 4-stage pipeline
// (framebuffer word -> byte -> palette entry -> output)
//
    reg [9:0]   x_count;
    reg [9:0]   y_count;
    reg [15:0]  pix_addr;

    wire        active = x_count >= H_BPORCH && x_count < H_BPORCH + H_ACTIVE &&
                         y_count >= V_BPORCH && y_count < V_BPORCH + V_ACTIVE;

    reg [31:0]  fb_q;
    reg [7:0]   index;
    reg [23:0]  pal_q;
    reg [1:0]   sel_a;
    reg [2:0]   de_d, hs_d, vs_d;

always @(posedge clk_vid or negedge reset_n) begin
    if(~reset_n) begin
        x_count <= 0;
        y_count <= 0;
        pix_addr <= 0;
        de_d <= 0;
        hs_d <= 0;
        vs_d <= 0;
        vid_de <= 0;
        vid_hs <= 0;
        vid_vs <= 0;
        vid_rgb <= 0;
    end else begin
        x_count <= x_count + 1'b1;
        if(x_count == H_TOTAL-1) begin
            x_count <= 0;
            y_count <= y_count + 1'b1;
            if(y_count == V_TOTAL-1) y_count <= 0;
        end

        if(x_count == 0 && y_count == 0) pix_addr <= 0;
        else if(active) pix_addr <= pix_addr + 1'b1;

        // sync and data enable delayed to match; VS at the frame's first clock,
        // HS a few clocks later, both in the back porch
        de_d <= {de_d[1:0], active};
        vs_d <= {vs_d[1:0], x_count == 0 && y_count == 0};
        hs_d <= {hs_d[1:0], x_count == 3};

        // stage 4: output, black outside the visible area
        vid_de <= de_d[2];
        vid_vs <= vs_d[2];
        vid_hs <= hs_d[2];
        vid_rgb <= de_d[2] ? pal_q : 24'h0;
    end
end

// the RAM reads stay out of the reset block so they map onto M10K blocks
always @(posedge clk_vid) begin
    // stage 1: framebuffer word
    fb_q <= fb[pix_addr[15:2]];
    sel_a <= pix_addr[1:0];
    // stage 2: the pixel's byte
    case(sel_a)
        2'd0: index <= fb_q[31:24];
        2'd1: index <= fb_q[23:16];
        2'd2: index <= fb_q[15:8];
        2'd3: index <= fb_q[7:0];
    endcase
    // stage 3: palette entry
    pal_q <= pal[index];
end

endmodule
