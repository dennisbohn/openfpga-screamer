// soc testbench: the CPU system of step 1 with its firmware, loaded the way the Pocket
// loads it, checked the way docs/soc.md describes.
//
// Usage: Vsoc_system <firmware.bin> <out prefix> [timeout in frames, default 20]
// Writes <prefix>.rgb (raw 320x200 RGB of the checked frame) and <prefix>_log.txt
// (the debug log as the APF would save it as debug.txt).
//
// Sequence: hold reset, fill the debug log slot with 0xFF (what the APF does when
// debug.txt does not exist), load firmware.bin to bridge address 0, release reset.
// Run until the firmware reports through SYS_SIM, then capture two more frames and
// check: SYS_SIM = 0x80000007, the test band of the last frame against the palette
// formula, the debug log's slot size in the slot table, and print the log.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vsoc_system.h"

static const int W = 320, H = 200;
static const uint64_t HALF_74 = 6734, HALF_VID = 40690;    // ps
static const uint64_t FRAME_PS = 400ull * 512 * 2 * HALF_VID;

struct Sim {
    Vsoc_system top;
    uint64_t now = 0, next_74 = HALF_74, next_vid = HALF_VID;

    // one clock edge, whichever is next; returns 1 for a rising clk_74a edge, 2 for a rising clk_vid edge
    int step() {
        if (next_74 <= next_vid) {
            now = next_74; next_74 += HALF_74;
            top.clk_74a = !top.clk_74a; top.eval();
            if (top.clk_74a) return 1;
        } else {
            now = next_vid; next_vid += HALF_VID;
            top.clk_vid = !top.clk_vid; top.eval();
            if (top.clk_vid) return 2;
        }
        return 0;
    }
    void rise_74(int n = 1) { while (n > 0) if (step() == 1) n--; }

    void bridge_write(uint32_t addr, uint32_t data) {
        rise_74(3);
        top.bridge_addr = addr; top.bridge_wr_data = data; top.bridge_wr = 1; top.eval();
        rise_74();
        top.bridge_wr = 0; top.eval();
    }
    uint32_t bridge_read(uint32_t addr) {
        top.bridge_addr = addr; top.eval();
        rise_74(4);
        return top.bridge_log_rd_data;
    }
};

static uint32_t pal_formula(int i) {
    return uint32_t(i) << 16 | uint32_t(255 - i) << 8 | uint32_t((i * 3) & 255);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s firmware.bin outprefix [timeout frames]\n", argv[0]); return 2; }
    int timeout_frames = argc > 3 ? atoi(argv[3]) : 20;

    std::vector<uint8_t> fw;
    {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fw.insert(fw.end(), buf, buf + n);
        fclose(f);
    }
    if (fw.empty() || fw.size() > 16384) { fprintf(stderr, "firmware size %zu is not 1..16384\n", fw.size()); return 2; }
    while (fw.size() % 4) fw.push_back(0);

    Sim s;
    Vsoc_system &top = s.top;
    top.clk_74a = 0; top.clk_vid = 0; top.reset_n = 0; top.bridge_wr = 0; top.cont1_key = 0;
    top.eval();

    // as the APF does it: the nonvolatile slot first (no debug.txt yet: 0xFF), then the firmware;
    // the bridge sends big-endian words, the first file byte in bits 31:24
    for (uint32_t a = 0; a < 4096; a += 4) s.bridge_write(0x00100000 + a, 0xFFFFFFFF);
    for (size_t a = 0; a < fw.size(); a += 4)
        s.bridge_write(a, uint32_t(fw[a]) << 24 | fw[a + 1] << 16 | fw[a + 2] << 8 | fw[a + 3]);
    s.rise_74(16);
    top.reset_n = 1; top.eval();

    // run until SYS_SIM says done, then two more frames, capturing each
    std::vector<uint8_t> frame, last;
    int frames = 0, done_at = -1, prev_vs = 0;
    uint64_t limit = s.now + uint64_t(timeout_frames) * FRAME_PS;
    while (true) {
        if (s.step() != 2) continue;
        if (top.vid_vs && !prev_vs) {
            frames++;
            if (frame.size() == size_t(W * H * 3)) last = frame;
            frame.clear();
            if (done_at < 0 && (top.sim_status & 0x80000000u)) done_at = frames;
            if (done_at >= 0 && frames >= done_at + 2) break;
        }
        prev_vs = top.vid_vs;
        if (top.vid_de) {
            uint32_t c = top.vid_rgb;
            frame.push_back(c >> 16); frame.push_back(c >> 8); frame.push_back(c);
        }
        if (s.now > limit) break;
    }

    int fails = 0;
    uint32_t st = top.sim_status;
    printf("SYS_SIM = 0x%08x after %d frames\n", st, frames);
    if (done_at < 0) { printf("FAIL: the firmware never reported (timeout %d frames)\n", timeout_frames); fails++; }
    else if (st & 0x40000000u) { printf("FAIL: trap, mcause %u\n", st & 0x3FFFFFFFu); fails++; }
    else if (st != 0x80000007u) {
        printf("FAIL: self-tests:%s%s%s\n", st & 1 ? "" : " integer", st & 2 ? "" : " float", st & 4 ? "" : " CFU");
        fails++;
    } else printf("self-tests: integer, float and CFU pass\n");

    // the test band of the last whole frame
    if (last.size() == size_t(W * H * 3)) {
        int bad = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 256; x++) {
                uint32_t want = pal_formula(x);
                const uint8_t *p = &last[(y * W + x) * 3];
                uint32_t got = uint32_t(p[0]) << 16 | p[1] << 8 | p[2];
                if (got != want) { if (bad < 5) printf("band pixel %d,%d: %06x, expected %06x\n", x, y, got, want); bad++; }
            }
        printf("test band: %s\n", bad ? (std::to_string(bad) + " of 2048 pixels wrong").c_str() : "all 2048 pixels match");
        fails += bad != 0;
        std::string name = std::string(argv[2]) + ".rgb";
        FILE *f = fopen(name.c_str(), "wb");
        fwrite(last.data(), 1, last.size(), f);
        fclose(f);
    } else { printf("FAIL: no whole frame captured\n"); fails++; }

    // the slot table entry and the log, read through the bridge as the APF does on exit
    printf("debug log slot size in the slot table: %u\n", top.log_slot_size);
    if (top.log_slot_size != 4096) { printf("FAIL: expected 4096\n"); fails++; }
    std::string log;
    for (uint32_t a = 0; a < 4096; a += 4) {
        uint32_t w = s.bridge_read(0x00100000 + a);
        for (int k = 3; k >= 0; k--) log.push_back(char(w >> (8 * k)));
    }
    {
        std::string name = std::string(argv[2]) + "_log.txt";
        FILE *f = fopen(name.c_str(), "wb");
        fwrite(log.data(), 1, log.size(), f);
        fclose(f);
    }
    size_t end = log.find('\0');
    printf("---- debug.txt (%zu bytes of text) ----\n%s\n---- end ----\n", end == std::string::npos ? log.size() : end,
           log.substr(0, end).c_str());
    if (end == 0 || end == std::string::npos) { printf("FAIL: the log is empty or not terminated\n"); fails++; }

    if (fails) { printf("FAIL\n"); return 1; }
    printf("PASS: firmware ran, self-tests pass, test band matches, log readable over the bridge, slot size set\n");
    return 0;
}
