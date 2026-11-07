//
// Test writer for sim/display, in place of the CPU. It copies two pictures from the bridge into the
// two framebuffers and their palettes, and a rising edge of the button asks video_out for a swap.
// Its swap request is cleared by reset, as soc.v's is.
//
// Bridge window (each picture has the layout of screen.bin, see tools/make_screen.py):
//   0x0000_0000  buffer 0: palette 0x000-0x3FF (R,G,B,0 words), pixels from 0x400
//   0x0010_0000  buffer 1: the same
// A rising edge on swap_button asks video_out for a swap, unless one is still pending.
//

module fb_bridge (
    input   wire            clk_74a,
    input   wire            reset_n,

    input   wire            bridge_wr,
    input   wire    [31:0]  bridge_addr,
    input   wire    [31:0]  bridge_wr_data,
    input   wire            swap_button,

    output  reg             fb_wr_en = 0,
    output  reg             fb_wr_buf,
    output  reg     [13:0]  fb_wr_addr,
    output  reg     [31:0]  fb_wr_data,

    output  reg             pal_wr = 0,
    output  reg             pal_buf,
    output  reg     [7:0]   pal_addr,
    output  reg     [23:0]  pal_data,

    output  reg             swap_req_toggle = 0,
    input   wire            swap_ack_toggle     // clk_vid domain
);

    localparam  FB_WORDS = 320 * 200 / 4;

    // bridge_addr[20] selects the buffer; the words of its picture are bridge_addr[15:2]. The
    // pixels start at byte 0x400, which is word 256, so fb_word is the pixel word's index.
    wire        in_window = bridge_addr[31:21] == 0 && bridge_addr[19:16] == 0;
    wire [13:0] fb_word = bridge_addr[15:2] - 14'd256;

// A bridge write into the window becomes a palette write (bytes 0x000-0x3FF; the word is R, G, B,
// 0, so the entry is bits 31:8) or a framebuffer write (one word of four pixels).
always @(posedge clk_74a) begin
    fb_wr_en <= 0;
    pal_wr <= 0;
    if(bridge_wr && in_window) begin
        if(bridge_addr[15:10] == 0) begin
            pal_wr <= 1;
            pal_buf <= bridge_addr[20];
            pal_addr <= bridge_addr[9:2];
            pal_data <= bridge_wr_data[31:8];
        end else if(fb_word < FB_WORDS) begin
            fb_wr_en <= 1;
            fb_wr_buf <= bridge_addr[20];
            fb_wr_addr <= fb_word;
            fb_wr_data <= bridge_wr_data;
        end
    end
end

    // The swap request is a toggle. A new request is made only when video_out has acknowledged the
    // last one, that is when the request toggle equals the synchronised acknowledge toggle.
    reg [1:0]   ack_s = 0;
    reg         button_d = 0;
    reg         reset = 1;                  // reset_n registered and inverted, as soc.v's cpu_reset

always @(posedge clk_74a) begin
    ack_s <= {ack_s[0], swap_ack_toggle};
    button_d <= swap_button;
    reset <= ~reset_n;
    if(reset) swap_req_toggle <= 0;
    else if(swap_button && !button_d && swap_req_toggle == ack_s[1])
        swap_req_toggle <= ~swap_req_toggle;
end

endmodule
