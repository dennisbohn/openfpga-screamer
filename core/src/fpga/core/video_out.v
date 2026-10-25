//
// 320x200 video out of the front framebuffer through its palette, at 60 Hz
//
// There are two framebuffers (framebuffer.v) and two palettes, one for each. The
// front one is shown; a writer draws the other and then asks for a swap by flipping
// swap_req_toggle. The swap happens at the start of the next line of the vertical
// blanking (312 of the 512 lines), at most once per blanking so no drawn frame is
// skipped; a frame never mixes the two buffers. swap_ack_toggle then follows
// swap_req_toggle, and the old front buffer is free to draw.
//
// Video timing at the 12.288 MHz pixel clock: 400 clocks x 512 lines = 60 Hz,
// 320 x 200 of it visible.
//

module video_out (
    // palette writes, synchronous to clk_74a
    input   wire            clk_74a,
    input   wire            pal_wr,
    input   wire            pal_buf,
    input   wire    [7:0]   pal_addr,
    input   wire    [23:0]  pal_data,

    // swap request, from another clock domain
    input   wire            swap_req_toggle,
    output  reg             swap_ack_toggle,
    output  reg             front,
    output  reg             frame_toggle,       // flips at the start of every frame

    // framebuffer read port, data one clock after the address
    output  wire            fb_rd_buf,
    output  wire    [13:0]  fb_rd_addr,
    input   wire    [31:0]  fb_rd_data,

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

    reg [23:0]  pal [0:511];

always @(posedge clk_74a) begin
    if(pal_wr) pal[{pal_buf, pal_addr}] <= pal_data;
end

//
// counters, swap, then a 4-stage pipeline
// (framebuffer word -> byte -> palette entry -> output)
//
    reg [9:0]   x_count;
    reg [9:0]   y_count;
    reg [15:0]  pix_addr;
    reg [1:0]   swap_s;

    wire        active = x_count >= H_BPORCH && x_count < H_BPORCH + H_ACTIVE &&
                         y_count >= V_BPORCH && y_count < V_BPORCH + V_ACTIVE;
    wire        frame_start = x_count == 0 && y_count == 0;
    wire        blank_line = y_count < V_BPORCH || y_count >= V_BPORCH + V_ACTIVE;
    reg         swap_armed;

    assign fb_rd_buf = front;
    assign fb_rd_addr = pix_addr[15:2];

    reg [1:0]   sel_a;
    reg [7:0]   index;
    reg [23:0]  pal_q;
    reg [2:0]   de_d, hs_d, vs_d;

always @(posedge clk_vid or negedge reset_n) begin
    if(~reset_n) begin
        x_count <= 0;
        y_count <= 0;
        pix_addr <= 0;
        front <= 0;
        swap_ack_toggle <= 0;
        swap_armed <= 1;
        frame_toggle <= 0;
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

        if(frame_start) begin
            pix_addr <= 0;
            frame_toggle <= ~frame_toggle;
        end
        else if(active) pix_addr <= pix_addr + 1'b1;

        // a pending swap takes effect at the start of a blanking line, once per
        // blanking: re-armed by the first visible line
        if(x_count == 0 && y_count == V_BPORCH) swap_armed <= 1;
        if(x_count == 0 && blank_line && swap_armed && swap_s[1] != swap_ack_toggle) begin
            front <= ~front;
            swap_ack_toggle <= swap_s[1];
            swap_armed <= 0;
        end

        // sync and data enable delayed to match the pipeline; VS at the frame's
        // first clock, HS a few clocks later, both in the back porch
        de_d <= {de_d[1:0], active};
        vs_d <= {vs_d[1:0], frame_start};
        hs_d <= {hs_d[1:0], x_count == 3};

        // output, black outside the visible area
        vid_de <= de_d[2];
        vid_vs <= vs_d[2];
        vid_hs <= hs_d[2];
        vid_rgb <= de_d[2] ? pal_q : 24'h0;
    end
end

// the RAM reads stay out of the reset block so they map onto M10K blocks
always @(posedge clk_vid) begin
    swap_s <= {swap_s[0], swap_req_toggle};
    // stage 1: framebuffer word arrives (read in framebuffer.v)
    sel_a <= pix_addr[1:0];
    // stage 2: the pixel's byte
    case(sel_a)
        2'd0: index <= fb_rd_data[31:24];
        2'd1: index <= fb_rd_data[23:16];
        2'd2: index <= fb_rd_data[15:8];
        2'd3: index <= fb_rd_data[7:0];
    endcase
    // stage 3: palette entry of the front buffer
    pal_q <= pal[{front, index}];
end

endmodule
