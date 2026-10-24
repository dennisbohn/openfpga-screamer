// fb_system testbench: framebuffer in the PSRAM model, loaded through the bridge
// while the video already runs, then one frame compared pixel by pixel.
//
// Usage: Vfb_system <screen.bin> <out.rgb> [bridge word spacing in clk_74a cycles, default 50]
// out.rgb gets the captured frame as raw 320x200 RGB bytes.
//
// The two clocks run at their real rates (74.25 MHz and 12.288 MHz) on a picosecond
// time line, so line reads and bridge writes interleave as on the Pocket.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "Vfb_system.h"

static const int W = 320, H = 200;
static const uint64_t HALF_74 = 6734, HALF_VID = 40690;    // ps

struct Sim {
    Vfb_system top;
    uint64_t now = 0, next_74 = HALF_74, next_vid = HALF_VID;
    uint64_t edges_74 = 0;
    // called on every rising clk_vid edge, after eval
    std::vector<uint8_t> frame;
    int vs_count = 0, capture_at = -1;

    // advance to the next clock edge; returns true if it was a rising clk_74a edge
    bool step() {
        bool rose74 = false;
        if (next_74 <= next_vid) {
            now = next_74; next_74 += HALF_74;
            top.clk_74a = !top.clk_74a;
            rose74 = top.clk_74a;
            if (rose74) edges_74++;
            top.eval();
        } else {
            now = next_vid; next_vid += HALF_VID;
            top.clk_vid = !top.clk_vid;
            top.eval();
            if (top.clk_vid) on_vid();
        }
        return rose74;
    }

    void on_vid() {
        if (top.vid_vs) vs_count++;
        if (vs_count == capture_at && top.vid_de) {
            uint32_t c = top.vid_rgb;
            frame.push_back(c >> 16); frame.push_back(c >> 8); frame.push_back(c);
        }
    }
};

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s screen.bin out.rgb [spacing]\n", argv[0]); return 2; }
    int spacing = argc > 3 ? atoi(argv[3]) : 50;

    std::vector<uint8_t> file(65024);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(file.data(), 1, file.size(), f) != file.size()) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fclose(f);

    Sim s;
    s.top.clk_74a = 0;
    s.top.clk_vid = 0;
    s.top.bridge_wr = 0;
    s.top.reset_n = 1;          // video runs from the start, the load happens underneath
    s.top.eval();

    // the APF writes the file as big-endian 32-bit words, one every `spacing` clk_74a cycles;
    // bridge signals change after a rising edge and are sampled at the next one
    size_t a = 0;
    uint64_t next_write = 100;
    while (a < file.size()) {
        if (s.step()) {
            s.top.bridge_wr = 0;
            if (s.edges_74 >= next_write) {
                s.top.bridge_addr = a;
                s.top.bridge_wr_data = file[a] << 24 | file[a + 1] << 16 | file[a + 2] << 8 | file[a + 3];
                s.top.bridge_wr = 1;
                a += 4;
                next_write += spacing;
            }
            s.top.eval();
        }
    }
    s.step(); s.top.bridge_wr = 0; s.top.eval();
    double load_ms = s.now / 1e9;

    // let the FIFO drain, then capture the second whole frame from now
    s.capture_at = s.vs_count + 2;
    while (s.vs_count <= s.capture_at) s.step();

    f = fopen(argv[2], "wb");
    fwrite(s.frame.data(), 1, s.frame.size(), f);
    fclose(f);

    printf("load took %.2f ms of simulated time (word every %d cycles)\n", load_ms, spacing);
    int fail = 0;
    if (s.top.error) { printf("FAIL: error flag (FIFO overflow or a line read too late)\n"); fail = 1; }
    if (s.top.psram_timing_error) { printf("FAIL: PSRAM write released before 70 ns\n"); fail = 1; }
    if (s.frame.size() != size_t(W * H * 3)) {
        printf("FAIL: captured %zu pixels, expected %d x %d\n", s.frame.size() / 3, W, H);
        return 1;
    }
    int bad = 0;
    for (int i = 0; i < W * H; i++) {
        const uint8_t *pal = &file[file[1024 + i] * 4];
        for (int k = 0; k < 3; k++)
            if (s.frame[i * 3 + k] != pal[k]) { if (bad < 5) printf("pixel %d,%d wrong\n", i % W, i / W); bad++; break; }
    }
    if (bad) {
        printf("FAIL: %d of %d pixels wrong; rows with wrong pixels (count):", bad, W * H);
        for (int y = 0, shown = 0; y < H && shown < 40; y++) {
            int n = 0;
            for (int x = 0; x < W; x++) {
                const uint8_t *pal = &file[file[1024 + y * W + x] * 4];
                const uint8_t *px = &s.frame[(y * W + x) * 3];
                n += px[0] != pal[0] || px[1] != pal[1] || px[2] != pal[2];
            }
            if (n) { printf(" %d(%d)", y, n); shown++; }
        }
        printf("\n");
        return 1;
    }
    if (fail) return 1;
    printf("PASS: %d x %d pixels, all match; no FIFO overflow, no late line, PSRAM timing kept\n", W, H);
    return 0;
}
