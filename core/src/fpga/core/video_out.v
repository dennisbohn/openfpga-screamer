//
// 320x200 video out from a two-line buffer through a 256-colour palette, at 60 Hz
//
// The framebuffer itself lives elsewhere (PSRAM, see fb_psram.v). Ahead of each
// visible line this module asks for it (line_req_toggle flips, line_req_y holds
// the line) and fb_psram writes it into one half of the line buffer while the
// other half is on screen.
//
// Video timing at the 12.288 MHz pixel clock: 400 clocks x 512 lines = 60 Hz,
// 320 x 200 of it visible.
//

module video_out (
    // palette and line buffer writes, synchronous to clk_74a
    input   wire            clk_74a,
    input   wire            pal_wr,
    input   wire    [7:0]   pal_addr,
    input   wire    [23:0]  pal_data,
    input   wire            lb_wr,
    input   wire    [8:0]   lb_addr,        // {line & 1, halfword 0..159}
    input   wire    [15:0]  lb_data,        // two pixels, the left one in [15:8]
    input   wire            error,          // shows a red top line when set

    // video out, synchronous to clk_vid
    input   wire            clk_vid,
    input   wire            reset_n,
    output  reg             line_req_toggle,
    output  reg     [7:0]   line_req_y,
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

    reg [15:0]  lb [0:511];
    reg [23:0]  pal [0:255];

always @(posedge clk_74a) begin
    if(pal_wr) pal[pal_addr] <= pal_data;
    if(lb_wr) lb[lb_addr] <= lb_data;
end

//
// read side: counters, then a 5-stage pipeline
// (address -> line buffer halfword -> byte -> palette entry -> output)
//
    reg [9:0]   x_count;
    reg [9:0]   y_count;

    wire        active = x_count >= H_BPORCH && x_count < H_BPORCH + H_ACTIVE &&
                         y_count >= V_BPORCH && y_count < V_BPORCH + V_ACTIVE;
    wire [9:0]  vis_x = x_count - H_BPORCH;
    wire [9:0]  vis_y = y_count - V_BPORCH;
    wire [9:0]  next_y = y_count - (V_BPORCH-1);

    reg [8:0]   lb_raddr;
    reg         sel_r;
    reg [15:0]  lb_q;
    reg         sel_a;
    reg [7:0]   index;
    reg [23:0]  pal_q;
    reg [3:0]   de_d, hs_d, vs_d, top_d;

    reg [1:0]   error_s;

always @(posedge clk_vid or negedge reset_n) begin
    if(~reset_n) begin
        x_count <= 0;
        y_count <= 0;
        line_req_toggle <= 0;
        line_req_y <= 0;
        de_d <= 0;
        hs_d <= 0;
        vs_d <= 0;
        top_d <= 0;
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

        // one line ahead: at the start of line y ask for the visible line that follows it
        if(x_count == 0 && y_count >= V_BPORCH-1 && y_count < V_BPORCH+V_ACTIVE-1) begin
            line_req_y <= next_y[7:0];
            line_req_toggle <= ~line_req_toggle;
        end

        // sync and data enable delayed to match the pipeline; VS at the frame's
        // first clock, HS a few clocks later, both in the back porch
        de_d <= {de_d[2:0], active};
        vs_d <= {vs_d[2:0], x_count == 0 && y_count == 0};
        hs_d <= {hs_d[2:0], x_count == 3};
        top_d <= {top_d[2:0], vis_y == 0};

        // output, black outside the visible area
        vid_de <= de_d[3];
        vid_vs <= vs_d[3];
        vid_hs <= hs_d[3];
        vid_rgb <= !de_d[3] ? 24'h0 : (error_s[1] && top_d[3]) ? 24'hFF0000 : pal_q;
    end
end

// the RAM reads stay out of the reset block so they map onto M10K blocks
always @(posedge clk_vid) begin
    error_s <= {error_s[0], error};
    // stage 0: address
    lb_raddr <= {vis_y[0], vis_x[8:1]};
    sel_r <= vis_x[0];
    // stage 1: halfword
    lb_q <= lb[lb_raddr];
    sel_a <= sel_r;
    // stage 2: the pixel's byte
    index <= sel_a ? lb_q[7:0] : lb_q[15:8];
    // stage 3: palette entry
    pal_q <= pal[index];
end

endmodule
