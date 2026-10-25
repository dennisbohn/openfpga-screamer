//
// Two 320x200 8-bit framebuffers in block RAM: video_out shows one while the other is drawn
//
// Both live in one memory of 2 x 16384 32-bit words (4 pixels a word, the leftmost in
// [31:24]); the buffer number is the top address bit. 128 M10K.
//   write port (clk_sys): one word with byte enables, so single pixels can be written
//   read port (clk_vid): data one clock after the address
//

module framebuffer (
    input   wire            clk_sys,
    input   wire            wr_en,
    input   wire            wr_buf,
    input   wire    [13:0]  wr_addr,        // word 0..15999
    input   wire    [31:0]  wr_data,
    input   wire    [3:0]   wr_be,          // [3] = leftmost pixel, [31:24]

    input   wire            clk_vid,
    input   wire            rd_buf,
    input   wire    [13:0]  rd_addr,
    output  reg     [31:0]  rd_data
);

    reg [31:0]  mem [0:32767];

// synthesis translate_off
    // simulation only: start from a poison pattern, so a lost write shows as a wrong pixel
    integer i;
    initial for(i = 0; i < 32768; i = i + 1) mem[i] = 32'hA5A5A5A5;
// synthesis translate_on

    wire [14:0] waddr = {wr_buf, wr_addr};

always @(posedge clk_sys) begin
    if(wr_en) begin
        if(wr_be[3]) mem[waddr][31:24] <= wr_data[31:24];
        if(wr_be[2]) mem[waddr][23:16] <= wr_data[23:16];
        if(wr_be[1]) mem[waddr][15:8]  <= wr_data[15:8];
        if(wr_be[0]) mem[waddr][7:0]   <= wr_data[7:0];
    end
end

always @(posedge clk_vid) begin
    rd_data <= mem[{rd_buf, rd_addr}];
end

endmodule
