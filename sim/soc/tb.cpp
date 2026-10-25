// soc testbench: the CPU system of step 1 with its firmware, loaded the way the Pocket
// loads it, checked the way docs/soc.md describes.
//
// Usage: Vsoc_system <firmware.bin> <out prefix> [timeout in frames, default 20]
//                    [expected SYS_SIM in hex, default 80000007]
// Writes <prefix>.rgb (raw 320x200 RGB of the checked frame) and <prefix>_log.txt
// (the debug log as the APF would save it as debug.txt); <prefix>_reset.* the same for
// the run after the reset.
//
// Sequence: hold reset, fill the debug log slot with 0xFF (what the APF does when
// debug.txt does not exist), check that the core ignored that, load firmware.bin to
// bridge address 0, release reset. Run until the firmware reports through SYS_SIM, then
// capture two more frames and check: SYS_SIM, the debug log's slot size in the slot
// table, the log's header line, and print the log. For a normal run also the test band
// of the last frame against the palette formula, then a reset in the middle of the run
// as when the firmware is reloaded from the menu: wait until buffer 1 is in front with
// no swap pending, reset, load firmware.bin again, release. In reset the swap request,
// the buffer in front and SYS_SIM must be 0, after it nothing may swap before the CPU
// asks, and the new run is checked like the first. For an expected trap (0xC0000000 |
// mcause, a fault test firmware) instead of the test band the trap handler's report: its
// box on screen and its TRAP line in the log.

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
    // one read transaction as io_bridge_peripheral.v does it: the address, 4 clocks, the
    // answer is sampled, then the bridge_rd pulse. The answer belongs to the previous read.
    uint32_t bridge_read(uint32_t addr) {
        top.bridge_addr = addr; top.eval();
        rise_74(4);
        uint32_t answer = top.bridge_log_rd_data;
        top.bridge_rd = 1; top.eval();
        rise_74();
        top.bridge_rd = 0; top.eval();
        rise_74(2);
        return answer;
    }
    // firmware.bin to bridge address 0 as the APF sends it: big-endian words, the first
    // file byte in bits 31:24
    void load_firmware(const std::vector<uint8_t> &fw) {
        for (size_t a = 0; a < fw.size(); a += 4)
            bridge_write(a, uint32_t(fw[a]) << 24 | fw[a + 1] << 16 | fw[a + 2] << 8 | fw[a + 3]);
    }

    // Run until SYS_SIM says done, then two more frames; returns the frames run and leaves
    // the last whole frame in last. done = false at the timeout. unasked: the buffer in
    // front changed before the CPU's swap request changed (since the call).
    int run(int timeout_frames, std::vector<uint8_t> &last, bool &done, bool &unasked) {
        std::vector<uint8_t> frame;
        int frames = 0, done_at = -1, prev_vs = top.vid_vs, prev_front = top.front, req0 = top.swap_req;
        bool asked = false;
        uint64_t limit = now + uint64_t(timeout_frames) * FRAME_PS;
        last.clear();
        unasked = false;
        while (now <= limit) {
            int e = step();
            if (!e) continue;
            asked |= top.swap_req != req0;
            if (top.front != prev_front) { unasked |= !asked; prev_front = top.front; }
            if (e != 2) continue;
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
        }
        done = done_at >= 0;
        return frames;
    }
};

static uint32_t pal_formula(int i) {
    return uint32_t(i) << 16 | uint32_t(255 - i) << 8 | uint32_t((i * 3) & 255);
}

// The checks after a run: SYS_SIM, the test band or the trap box, the slot size, the log.
// Writes <prefix>.rgb and <prefix>_log.txt; returns the number of failures.
static int check_run(Sim &s, const std::string &prefix, int frames, bool done, const std::vector<uint8_t> &last,
                     uint32_t expect, int timeout_frames) {
    Vsoc_system &top = s.top;
    bool expect_trap = (expect & 0xC0000000u) == 0xC0000000u;
    int fails = 0;
    uint32_t st = top.sim_status;
    printf("SYS_SIM = 0x%08x after %d frames, expected 0x%08x\n", st, frames, expect);
    if (!done) { printf("FAIL: the firmware never reported (timeout %d frames)\n", timeout_frames); fails++; }
    else if (st == expect) printf(expect_trap ? "trap as expected, mcause %u\n" : "self-tests: integer, float and CFU pass\n",
                                  st & 0x3FFFFFFFu);
    else if (st & 0x40000000u) { printf("FAIL: trap, mcause %u\n", st & 0x3FFFFFFFu); fails++; }
    else if (expect_trap) { printf("FAIL: no trap\n"); fails++; }
    else {
        printf("FAIL: self-tests:%s%s%s\n", st & 1 ? "" : " integer", st & 2 ? "" : " float", st & 4 ? "" : " CFU");
        fails++;
    }

    // the last whole frame: the test band, or the trap handler's box (rows 18..24 in
    // COL_ALERT; text row 22 is empty)
    if (last.size() == size_t(W * H * 3)) {
        int bad = 0, y0 = expect_trap ? 22 * 8 : 0, x1 = expect_trap ? W : 256;
        for (int y = y0; y < y0 + 8; y++)
            for (int x = 0; x < x1; x++) {
                uint32_t want = pal_formula(expect_trap ? 230 : x);
                const uint8_t *p = &last[(y * W + x) * 3];
                uint32_t got = uint32_t(p[0]) << 16 | p[1] << 8 | p[2];
                if (got != want) { if (bad < 5) printf("pixel %d,%d: %06x, expected %06x\n", x, y, got, want); bad++; }
            }
        int n = 8 * x1;
        printf("%s: %s\n", expect_trap ? "trap box" : "test band",
               bad ? (std::to_string(bad) + " of " + std::to_string(n) + " pixels wrong").c_str()
                   : ("all " + std::to_string(n) + " pixels match").c_str());
        fails += bad != 0;
        std::string name = prefix + ".rgb";
        FILE *f = fopen(name.c_str(), "wb");
        fwrite(last.data(), 1, last.size(), f);
        fclose(f);
    } else { printf("FAIL: no whole frame captured\n"); fails++; }

    // the slot table entry and the log, read through the bridge as the APF does on exit
    printf("debug log slot size in the slot table: %u\n", top.log_slot_size);
    if (top.log_slot_size != 4096) { printf("FAIL: expected 4096\n"); fails++; }
    // like the APF saving the slot: one read more than words, the first answer dropped
    std::string log;
    s.bridge_read(0x00100000);
    for (uint32_t a = 4; a <= 4096; a += 4) {
        uint32_t w = s.bridge_read(0x00100000 + a);
        for (int k = 3; k >= 0; k--) log.push_back(char(w >> (8 * k)));
    }
    {
        std::string name = prefix + "_log.txt";
        FILE *f = fopen(name.c_str(), "wb");
        fwrite(log.data(), 1, log.size(), f);
        fclose(f);
    }
    size_t end = log.find('\0');
    printf("---- debug.txt (%zu bytes of text) ----\n%s\n---- end ----\n", end == std::string::npos ? log.size() : end,
           log.substr(0, end).c_str());
    if (end == 0 || end == std::string::npos) { printf("FAIL: the log is empty or not terminated\n"); fails++; }
    else if (log.compare(0, 22, "Screamer Pocket step 1") != 0) { printf("FAIL: the log does not start with its header line\n"); fails++; }
    if (expect_trap) {
        char line[32];
        snprintf(line, sizeof line, "TRAP mcause=%08x", expect & 0x3FFFFFFFu);
        if (log.substr(0, end).find(line) == std::string::npos) { printf("FAIL: no \"%s\" in the log\n", line); fails++; }
    }
    return fails;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s firmware.bin outprefix [timeout frames] [expected SYS_SIM]\n", argv[0]); return 2; }
    int timeout_frames = argc > 3 ? atoi(argv[3]) : 20;
    uint32_t expect = argc > 4 ? uint32_t(strtoul(argv[4], nullptr, 16)) : 0x80000007u;
    bool expect_trap = (expect & 0xC0000000u) == 0xC0000000u;

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
    top.clk_74a = 0; top.clk_vid = 0; top.reset_n = 0; top.bridge_wr = 0; top.bridge_rd = 0; top.cont1_key = 0;
    top.eval();

    // as the APF does it: the nonvolatile slot first (no debug.txt yet: 0xFF), then the firmware
    for (uint32_t a = 0; a < 4096; a += 4) s.bridge_write(0x00100000 + a, 0xFFFFFFFF);
    int fails = 0;
    {
        // the core ignores that load (docs/soc.md), so the log still holds what it held
        int loaded = 0;
        s.bridge_read(0x00100000);
        for (uint32_t a = 4; a <= 64; a += 4) loaded += s.bridge_read(0x00100000 + a) == 0xFFFFFFFFu;
        printf("APF load of the debug log slot: %s\n", loaded ? "taken (FAIL)" : "ignored");
        fails += loaded != 0;
    }
    s.load_firmware(fw);
    s.rise_74(16);
    top.reset_n = 1; top.eval();

    std::vector<uint8_t> last;
    bool done, unasked;
    int frames = s.run(timeout_frames, last, done, unasked);
    fails += check_run(s, argv[2], frames, done, last, expect, timeout_frames);

    if (!expect_trap) {
        // a reset in the middle of the run, as when the firmware is reloaded from the menu
        // (Reset Enter, the slot load, Reset Exit), while buffer 1 is in front
        printf("==== reset in the middle of the run, firmware loaded again ====\n");
        uint64_t limit = s.now + 4 * FRAME_PS;
        while (!(top.swap_req && top.front) && s.now < limit) s.step();
        if (!(top.swap_req && top.front)) { printf("FAIL: buffer 1 never in front with no swap pending\n"); fails++; }
        top.reset_n = 0; top.eval();
        s.rise_74(16);
        printf("in reset: swap request %d, buffer in front %d, SYS_SIM 0x%08x\n", top.swap_req, top.front, top.sim_status);
        if (top.swap_req || top.front || top.sim_status) { printf("FAIL: all three should be 0\n"); fails++; }
        s.load_firmware(fw);
        s.rise_74(16);
        top.reset_n = 1; top.eval();
        frames = s.run(timeout_frames, last, done, unasked);
        if (unasked) { printf("FAIL: a swap after the reset that the CPU did not ask for\n"); fails++; }
        else printf("after the reset: buffer 0 in front until the CPU's first swap\n");
        fails += check_run(s, std::string(argv[2]) + "_reset", frames, done, last, expect, timeout_frames);
    }

    if (fails) { printf("FAIL\n"); return 1; }
    printf(expect_trap ? "PASS: the trap was taken and reported on screen and in the log, slot size set\n"
                       : "PASS: firmware ran, self-tests pass, test band matches, log readable over the bridge, slot size set; "
                         "the same after a reset in the middle of the run, which swapped nothing by itself\n");
    return 0;
}
