// fb_video testbench: load screen.bin through the bridge write port, run one video
// frame and compare every visible pixel with the palette lookup done here in C++.
//
// Usage: Vfb_video <screen.bin> <out.rgb>
// out.rgb gets the captured frame as raw 320x200 RGB bytes.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vfb_video.h"

static const int W = 320, H = 200;

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s screen.bin out.rgb\n", argv[0]); return 2; }

    std::vector<uint8_t> file(65024);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(file.data(), 1, file.size(), f) != file.size()) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fclose(f);

    Vfb_video top;
    top.clk_74a = 0;
    top.clk_vid = 0;
    top.bridge_wr = 0;
    top.reset_n = 0;
    top.eval();

    // the APF writes the file as big-endian 32-bit words from the slot's address
    for (size_t a = 0; a < file.size(); a += 4) {
        top.bridge_addr = a;
        top.bridge_wr_data = file[a] << 24 | file[a + 1] << 16 | file[a + 2] << 8 | file[a + 3];
        top.bridge_wr = 1;
        top.clk_74a = 1; top.eval();
        top.clk_74a = 0; top.eval();
    }
    top.bridge_wr = 0;
    top.eval();

    // video: release reset, then capture from the second VS on (one whole frame)
    top.reset_n = 1;
    top.eval();
    std::vector<uint8_t> got;
    int vs_seen = 0, lines = 0, prev_de = 0;
    for (long cycle = 0; cycle < 3L * 400 * 512 && vs_seen < 3; cycle++) {
        top.clk_vid = 1; top.eval();
        if (top.vid_vs) vs_seen++;
        if (vs_seen == 2 && top.vid_de) {
            uint32_t c = top.vid_rgb;
            got.push_back(c >> 16); got.push_back(c >> 8); got.push_back(c);
        }
        if (vs_seen == 2 && prev_de && !top.vid_de) lines++;
        prev_de = top.vid_de;
        top.clk_vid = 0; top.eval();
    }

    f = fopen(argv[2], "wb");
    fwrite(got.data(), 1, got.size(), f);
    fclose(f);

    if (got.size() != size_t(W * H * 3)) {
        printf("FAIL: captured %zu pixels in %d lines, expected %d x %d\n", got.size() / 3, lines, W, H);
        return 1;
    }
    int bad = 0;
    for (int i = 0; i < W * H; i++) {
        const uint8_t *pal = &file[file[1024 + i] * 4];
        for (int k = 0; k < 3; k++)
            if (got[i * 3 + k] != pal[k]) { if (bad < 5) printf("pixel %d,%d wrong\n", i % W, i / W); bad++; break; }
    }
    if (bad) { printf("FAIL: %d of %d pixels wrong\n", bad, W * H); return 1; }
    printf("PASS: %d x %d pixels in %d lines, all match\n", W, H, lines);
    return 0;
}
