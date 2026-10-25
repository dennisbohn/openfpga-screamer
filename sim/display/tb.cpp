// display testbench: two pictures into the two framebuffers through the bridge, button
// presses that swap them, a reset; every frame but the one cut short by the reset is
// compared pixel by pixel. Then the framebuffer's drawing port: read back, and byte-enable
// writes.
//
// Usage: Vdisplay <screen.bin> <screen2.bin> <out prefix>
// Writes <prefix>N.rgb (raw 320x200 RGB) for every captured frame.
//
// Schedule (frames counted from the first VS after reset; "line" = visible lines done):
//   frame 1          A
//   frame 2          A, button A pressed at line 100 -> the swap waits for the blanking
//   frame 3          B
//   frame 4          B, pressed at line 50
//   frame 5          A
//   frame 6          A, pressed at line 20 and again at line 120 while the first is pending
//   frame 7, 8       B, B (the second press was ignored)
//   frame 9          B, reset_n held low from line 100 for 500 pixel clocks (cut short, not checked)
//   frame 10         A: after a reset buffer 0 is in front and nothing swaps (fb_bridge
//                    resets its request as soc.v does), pressed at line 50
//   frame 11         B, pressed in the blanking after the last line: the swap must follow
//                    within two lines; pressed again 2000 clocks later, still in the same
//                    blanking: that swap must wait for the next blanking
//   frame 12         A
//   frame 13         B

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vdisplay.h"

static const int W = 320, H = 200;
static const uint64_t HALF_74 = 6734, HALF_VID = 40690;    // ps
static const uint64_t LINE_PS = 480 * 2 * HALF_VID;        // one video line

static std::vector<uint8_t> load(const char *path) {
    std::vector<uint8_t> f(65024);
    FILE *fp = fopen(path, "rb");
    if (!fp || fread(f.data(), 1, f.size(), fp) != f.size()) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    fclose(fp);
    return f;
}

static uint32_t word_of(const std::vector<uint8_t> &f, size_t w) {
    const uint8_t *p = &f[1024 + 4 * w];
    return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

struct Sim {
    Vdisplay top;
    uint64_t now = 0, next_74 = HALF_74, next_vid = HALF_VID, edges_74 = 0;

    // one clock edge, whichever is next; returns 1 for a rising clk_74a edge, 2 for a rising clk_vid edge
    int step() {
        if (next_74 <= next_vid) {
            now = next_74; next_74 += HALF_74;
            top.clk_74a = !top.clk_74a; top.eval();
            if (top.clk_74a) { edges_74++; return 1; }
        } else {
            now = next_vid; next_vid += HALF_VID;
            top.clk_vid = !top.clk_vid; top.eval();
            if (top.clk_vid) return 2;
        }
        return 0;
    }
    void rise_74() { while (step() != 1) {} }
};

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s screen.bin screen2.bin outprefix\n", argv[0]); return 2; }
    std::vector<uint8_t> img[2] = { load(argv[1]), load(argv[2]) };

    Sim s;
    Vdisplay &top = s.top;
    top.clk_74a = 0; top.clk_vid = 0; top.reset_n = 0; top.bridge_wr = 0; top.swap_button = 0; top.tb_sys = 0;
    top.eval();

    // load both pictures: picture n to bridge address n * 0x100000, a word every 4 clocks
    for (int pic = 0; pic < 2; pic++)
        for (size_t a = 0; a < img[pic].size(); a += 4) {
            for (int k = 0; k < 4; k++) { s.rise_74(); top.bridge_wr = 0; top.eval(); }
            const std::vector<uint8_t> &f = img[pic];
            top.bridge_addr = pic * 0x100000 + a;
            top.bridge_wr_data = f[a] << 24 | f[a + 1] << 16 | f[a + 2] << 8 | f[a + 3];
            top.bridge_wr = 1; top.eval();
        }
    s.rise_74();                                // the last word is taken here
    top.bridge_wr = 0; top.eval();

    // run the video; capture frames, press the button and reset on schedule
    top.reset_n = 1; top.eval();
    struct Press { int frame, line; long after; };   // after > 0: clocks after the previous press
    const Press presses[] = { {2, 100, 0}, {4, 50, 0}, {6, 20, 0}, {6, 120, 0}, {10, 50, 0},
                              {11, 200, 0}, {-1, -1, 2000} };
    const int NPRESS = sizeof presses / sizeof presses[0];
    const int expect[] = { -1, 0, 0, 1, 1, 0, 0, 1, 1, -1, 0, 1, 0, 1 };   // picture per frame 1..13, -1: not checked
    const int FRAMES = 13;
    const int RESET_FRAME = 9, RESET_LINE = 100, RESET_CLOCKS = 500;
    const int BLANK_PRESS = 5;                  // index of the press in the blanking

    std::vector<std::vector<uint8_t>> frames(FRAMES + 1);
    int frame = 0, line = 0, prev_de = 0, prev_front = top.front;
    int next_press = 0, reset_left = -1;
    long press_left = 0;
    uint64_t last_press = 0, blank_press_ps = 0, blank_swap_ps = 0;
    while (frame <= FRAMES) {
        int e = s.step();
        if (e == 1) {
            if (press_left > 0 && --press_left == 0) { top.swap_button = 0; top.eval(); }
            if (next_press < NPRESS && press_left == 0) {
                const Press &p = presses[next_press];
                bool go = p.after > 0 ? s.edges_74 - last_press >= uint64_t(p.after)
                                      : frame == p.frame && line == p.line;
                if (go) {
                    top.swap_button = 1; top.eval();
                    press_left = 200;           // held for 200 clocks
                    last_press = s.edges_74;
                    if (next_press == BLANK_PRESS) blank_press_ps = s.now;
                    next_press++;
                }
            }
        } else if (e == 2) {
            if (reset_left > 0 && --reset_left == 0) { top.reset_n = 1; top.eval(); }
            if (reset_left < 0 && frame == RESET_FRAME && line == RESET_LINE) {
                top.reset_n = 0; top.eval();
                reset_left = RESET_CLOCKS;
            }
            if (top.front != prev_front) {
                if (blank_press_ps && !blank_swap_ps) blank_swap_ps = s.now;
                prev_front = top.front;
            }
            if (top.vid_vs) { frame++; line = 0; }
            if (frame >= 1 && frame <= FRAMES && top.vid_de) {
                uint32_t c = top.vid_rgb;
                frames[frame].push_back(c >> 16); frames[frame].push_back(c >> 8); frames[frame].push_back(c);
            }
            if (prev_de && !top.vid_de) line++;
            prev_de = top.vid_de;
        }
    }

    int fails = 0;
    for (int k = 1; k <= FRAMES; k++) {
        std::string name = std::string(argv[3]) + std::to_string(k) + ".rgb";
        FILE *fp = fopen(name.c_str(), "wb");
        fwrite(frames[k].data(), 1, frames[k].size(), fp);
        fclose(fp);
        if (expect[k] < 0) { printf("frame %d: interrupted by the reset, not checked\n", k); continue; }

        const std::vector<uint8_t> &f = img[expect[k]];
        int bad = 0;
        if (frames[k].size() != size_t(W * H * 3)) bad = W * H;
        else for (int i = 0; i < W * H; i++) {
            const uint8_t *pal = &f[f[1024 + i] * 4];
            if (memcmp(&frames[k][i * 3], pal, 3) != 0) bad++;
        }
        printf("frame %d: expected picture %c, %s\n", k, 'A' + expect[k],
               bad ? (std::to_string(bad) + " of 64000 pixels wrong").c_str() : "all 64000 pixels match");
        fails += bad != 0;
    }

    double lines = blank_swap_ps ? double(blank_swap_ps - blank_press_ps) / LINE_PS : 1e9;
    printf("press in the blanking: swapped after %.2f lines\n", lines);
    if (lines > 2.0) { printf("  -> too late, should be within 2 lines\n"); fails++; }

    // the drawing port: read back words of both pictures, then byte-enable writes
    top.tb_sys = 1; top.tb_we = 0; top.eval();
    int port_bad = 0;
    for (int pic = 0; pic < 2; pic++)
        for (size_t w = 0; w < 16000; w += 997) {
            top.tb_buf = pic; top.tb_addr = w; top.eval();
            s.rise_74();
            if (top.sys_rd_data != word_of(img[pic], w)) {
                if (port_bad < 5) printf("read buffer %d word %zu: %08x, expected %08x\n", pic, w, top.sys_rd_data, word_of(img[pic], w));
                port_bad++;
            }
        }
    const uint32_t old = word_of(img[1], 1234);
    top.tb_buf = 1; top.tb_addr = 1234; top.tb_wdata = 0x11223344; top.tb_be = 0xA; top.tb_we = 1; top.eval();
    s.rise_74();                                // write lanes 3 and 1; the read returns the new word
    const uint32_t merged = (0x11223344 & 0xFF00FF00) | (old & 0x00FF00FF);
    if (top.sys_rd_data != merged) { printf("write with byte enables: read %08x during the write, expected %08x\n", top.sys_rd_data, merged); port_bad++; }
    top.tb_we = 0; top.eval();
    s.rise_74();
    if (top.sys_rd_data != merged) { printf("write with byte enables: read %08x after, expected %08x\n", top.sys_rd_data, merged); port_bad++; }
    printf("drawing port: %s\n", port_bad ? "FAILED" : "reads match both pictures, byte enables write only their pixels");
    fails += port_bad != 0;

    if (fails) { printf("FAIL\n"); return 1; }
    printf("PASS: swaps only in the blanking and at most once per blanking, each picture with its own palette, "
           "a press while a swap is pending ignored, after a reset buffer 0 without a swap, drawing port reads and writes\n");
    return 0;
}
