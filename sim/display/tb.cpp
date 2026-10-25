// display testbench: two pictures into the two framebuffers through the bridge, then
// button presses that swap them; every frame is compared pixel by pixel.
//
// Usage: Vdisplay <screen.bin> <screen2.bin> <out prefix>
// Writes <prefix>N.rgb (raw 320x200 RGB) for every captured frame.
//
// Schedule (frames counted from the first VS after reset):
//   frame 1          A
//   frame 2          A, button A pressed at line 100 -> swap requested mid-frame
//   frame 3          B (the swap waits for the frame boundary)
//   frame 4          B, pressed again at line 50
//   frame 5          A
//   frame 6          A, pressed at line 20 and again at line 120 while the first is pending
//   frame 7, 8       B, B (the second press was ignored)
//   frame 9          B, reset_n held low from line 100 for 500 pixel clocks (frame cut short, not checked)
//   frame 10         B (a reset must not swap), pressed at line 50
//   frame 11         A

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "Vdisplay.h"

static const int W = 320, H = 200;
static const uint64_t HALF_74 = 6734, HALF_VID = 40690;    // ps

static std::vector<uint8_t> load(const char *path) {
    std::vector<uint8_t> f(65024);
    FILE *fp = fopen(path, "rb");
    if (!fp || fread(f.data(), 1, f.size(), fp) != f.size()) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    fclose(fp);
    return f;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s screen.bin screen2.bin outprefix\n", argv[0]); return 2; }
    std::vector<uint8_t> img[2] = { load(argv[1]), load(argv[2]) };

    Vdisplay top;
    top.clk_74a = 0; top.clk_vid = 0; top.reset_n = 0; top.bridge_wr = 0; top.swap_button = 0;
    top.eval();

    uint64_t next_74 = HALF_74, next_vid = HALF_VID;
    uint64_t edges_74 = 0;

    // load both pictures: picture n to bridge address n * 0x100000, a word every 4 clocks
    size_t pic = 0, a = 0;
    while (pic < 2) {
        if (next_74 <= next_vid) {
            next_74 += HALF_74;
            top.clk_74a = !top.clk_74a; top.eval();
            if (top.clk_74a) {
                edges_74++;
                top.bridge_wr = 0;
                if (edges_74 % 4 == 0) {
                    const std::vector<uint8_t> &f = img[pic];
                    top.bridge_addr = pic * 0x100000 + a;
                    top.bridge_wr_data = f[a] << 24 | f[a + 1] << 16 | f[a + 2] << 8 | f[a + 3];
                    top.bridge_wr = 1;
                    a += 4;
                    if (a == f.size()) { a = 0; pic++; }
                }
                top.eval();
            }
        } else {
            next_vid += HALF_VID;
            top.clk_vid = !top.clk_vid; top.eval();
        }
    }
    // the last word is sampled at the next rising edge; then the bridge goes quiet
    for (bool rose = false; !rose; ) {
        if (next_74 <= next_vid) {
            next_74 += HALF_74;
            top.clk_74a = !top.clk_74a; top.eval();
            rose = top.clk_74a;
        } else {
            next_vid += HALF_VID;
            top.clk_vid = !top.clk_vid; top.eval();
        }
    }
    top.bridge_wr = 0; top.eval();

    // run the video; capture frames, press the button on schedule
    top.reset_n = 1; top.eval();
    struct Press { int frame, line; };
    const Press presses[] = { {2, 100}, {4, 50}, {6, 20}, {6, 120}, {10, 50} };
    const int expect[] = { -1, 0, 0, 1, 1, 0, 0, 1, 1, -1, 1, 0 };     // picture per frame 1..11, -1: not checked
    const int FRAMES = 11;
    const int RESET_FRAME = 9, RESET_LINE = 100, RESET_CLOCKS = 500;
    int reset_left = -1;

    std::vector<std::vector<uint8_t>> frames(FRAMES + 1);
    int frame = 0, line = 0, prev_de = 0;
    size_t next_press = 0;
    long press_left = 0;
    while (frame <= FRAMES) {
        if (next_74 <= next_vid) {
            next_74 += HALF_74;
            top.clk_74a = !top.clk_74a; top.eval();
            if (top.clk_74a) {
                if (press_left > 0 && --press_left == 0) { top.swap_button = 0; top.eval(); }
                if (next_press < sizeof presses / sizeof presses[0] && press_left == 0 &&
                    frame == presses[next_press].frame && line == presses[next_press].line) {
                    top.swap_button = 1; top.eval();
                    press_left = 200;       // held for 200 clocks, released well before the next press
                    next_press++;
                }
            }
        } else {
            next_vid += HALF_VID;
            top.clk_vid = !top.clk_vid; top.eval();
            if (top.clk_vid) {
                if (reset_left > 0 && --reset_left == 0) { top.reset_n = 1; top.eval(); }
                if (reset_left < 0 && frame == RESET_FRAME && line == RESET_LINE) {
                    top.reset_n = 0; top.eval();
                    reset_left = RESET_CLOCKS;
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
    if (fails) { printf("FAIL: %d frames wrong\n", fails); return 1; }
    printf("PASS: swaps only at frame boundaries, each picture with its own palette, "
           "a press while a swap is pending ignored, a reset mid-frame keeps the shown picture\n");
    return 0;
}
