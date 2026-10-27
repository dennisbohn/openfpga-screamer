// soc testbench: the CPU system with its firmware and a model of the SDRAM, loaded the way
// the Pocket loads it, checked the way docs/soc.md and docs/speicher.md describe.
//
// Usage: Vsoc_system <firmware.bin> <out prefix> [timeout in frames, default 80]
//                    [expected SYS_SIM in hex, default 80001FFF]
// Writes <prefix>.rgb (raw 320x200 RGB of the checked frame) and <prefix>_log.txt
// (the debug log as the APF would save it as debug.txt); <prefix>_reset.* the same for
// the run after the resets.
//
// Loading like the APF: the core held in reset, a bridge word at most every 88 clocks
// (io_bridge_peripheral.v), firmware.bin to bridge address 0, the debug log slot filled
// with 0xFF (debug.txt does not exist yet; the core must ignore it), then the wait for the
// acknowledge core_top gives a slot write once the SDRAM is ready. Then reset is released.
// program.bin (next to firmware.bin) is deferload: the boot loader asks for it after the
// SDRAM tests (ds_read, core_bridge_cmd's target command 0x0180) and the testbench answers
// like the APF: done low, the words over the bridge, done high with the result (2 for a
// slot without a file, as the track's here). The boot loader checks program.bin and
// starts it.
//
// Run until the firmware reports through SYS_SIM, then capture two more frames and check:
// SYS_SIM, the model error counts and the longest refresh gap, the debug log's slot size
// and header line, and print the log. A normal run also checks the test band of the last
// frame and that program.bin passed its check. Then two resets as when the firmware is reloaded
// from the menu (only firmware.bin is loaded again): one while buffer 1 is in front (the
// swap request, the buffer in front and SYS_SIM must be 0 in reset), one in the middle of
// the memory test's SDRAM traffic. The last run must pass like the first with the
// program.bin of the first load and swap nothing the CPU did not ask for. For
// an expected trap (0xC0000000 | mcause, a fault test firmware) instead of the test band
// the trap handler's report: its box on screen and its TRAP line in the log. Any other
// expected value is a negative test (a fault built into a memory model, SIM_DEFINES in
// tools/sim.sh): SYS_SIM must show exactly those tests failed, the log must report a
// failure, and the resets are left out.

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vsoc_system.h"

static const int W = 320, H = 200;
static const uint64_t HALF_74 = 6734, HALF_VID = 40690;    // ps
static const uint64_t FRAME_PS = 480ull * 512 * 2 * HALF_VID;   // 50 Hz
static const int BRIDGE_GAP = 88;                           // clocks per bridge word, APF worst case

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

    void bridge_write(uint32_t addr, uint32_t data, int gap = BRIDGE_GAP) {
        rise_74(gap - 1);
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
    // a file to a bridge address as the APF sends it: big-endian words, the first file byte
    // in bits 31:24
    void load(uint32_t base, const std::vector<uint8_t> &f, int gap = BRIDGE_GAP) {
        for (size_t a = 0; a < f.size(); a += 4)
            bridge_write(base + a, uint32_t(f[a]) << 24 | f[a + 1] << 16 | f[a + 2] << 8 | f[a + 3], gap);
    }

    // the APF's side of a data slot read the core asked for: slot 2 is program.bin, slot 3
    // the track when SIM_TRACK names one (sent faster than the APF would, 4 MB take long),
    // any other slot has no file. The clocks it takes do not count as frames of run().
    std::vector<uint8_t> prog, track;
    int slot_reads = 0;
    void serve_read() {
        top.ds_done = 0; top.eval();
        rise_74(8);
        uint32_t id = top.ds_id, off = top.ds_offset, addr = top.ds_addr, len = top.ds_length, err = 2;
        const std::vector<uint8_t> *file = id == 2 ? &prog : id == 3 && !track.empty() ? &track : nullptr;
        if (file && off < file->size()) {
            size_t end = std::min<size_t>(file->size(), size_t(off) + len);
            load(addr, std::vector<uint8_t>(file->begin() + off, file->begin() + end), id == 3 ? 12 : BRIDGE_GAP);
            err = 0;
        }
        printf("data slot read %u: %u bytes from %u to 0x%08x, result %u\n", id, len, off, addr, err);
        slot_reads++;
        rise_74(8);
        top.ds_err = err; top.ds_done = 1; top.eval();
        while (top.ds_read) rise_74();
    }

    // Run until SYS_SIM says done, then two more frames; returns the frames run and leaves
    // the last whole frame in last. done = false at the timeout. unasked: the buffer in
    // front changed before the CPU's swap request changed (since the call). max_clocks
    // stops the run early (for a reset in the middle).
    int run(int timeout_frames, std::vector<uint8_t> &last, bool &done, bool &unasked, uint64_t max_clocks = 0) {
        std::vector<uint8_t> frame;
        int frames = 0, done_at = -1, prev_vs = top.vid_vs, prev_front = top.front, req0 = top.swap_req;
        bool asked = false;
        uint64_t limit = now + uint64_t(timeout_frames) * FRAME_PS, clocks = 0;
        last.clear();
        unasked = false;
        while (now <= limit) {
            int e = step();
            if (!e) continue;
            if (e == 1 && top.ds_read) serve_read();
            asked |= top.swap_req != req0;
            if (top.front != prev_front) { unasked |= !asked; prev_front = top.front; }
            if (e == 1 && max_clocks && ++clocks >= max_clocks) break;
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

// The checks after a run: SYS_SIM, the models, the test band or the trap box, the slot
// size, the log. Writes <prefix>.rgb and <prefix>_log.txt; returns the number of failures.
static int check_run(Sim &s, const std::string &prefix, int frames, bool done, const std::vector<uint8_t> &last,
                     uint32_t expect, int timeout_frames, const char *slot_line) {
    Vsoc_system &top = s.top;
    bool expect_trap = (expect & 0xC0000000u) == 0xC0000000u;
    bool negative = !expect_trap && expect != 0x80001FFFu;
    int fails = 0;
    uint32_t st = top.sim_status;
    printf("SYS_SIM = 0x%08x after %d frames, expected 0x%08x\n", st, frames, expect);
    if (!done) { printf("FAIL: the firmware never reported (timeout %d frames)\n", timeout_frames); fails++; }
    else if (st == expect) printf(expect_trap ? "trap as expected, mcause %u\n"
                                  : negative ? "exactly the expected tests fail\n" : "all tests pass\n", st & 0x3FFFFFFFu);
    else if (st & 0x40000000u) { printf("FAIL: trap, mcause %u\n", st & 0x3FFFFFFFu); fails++; }
    else if (expect_trap) { printf("FAIL: no trap\n"); fails++; }
    else { printf("FAIL: failed test bits 0x%08x (see docs/soc.md)\n", ~st & expect & 0x3FFFFFFFu); fails++; }

    printf("model errors: SDRAM %u; longest SDRAM refresh gap %u clocks\n",
           top.sdram_errors, top.sdram_max_refresh);
    if (top.sdram_errors) { printf("FAIL: the memory model reported errors\n"); fails++; }
    if (top.sdram_max_refresh > 580) { printf("FAIL: refresh gap above 7.8 us (580 clocks)\n"); fails++; }

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
    std::string text = log.substr(0, end);
    printf("---- debug.txt (%zu bytes of text) ----\n%s\n---- end ----\n", text.size(), text.c_str());
    if (end == 0 || end == std::string::npos) { printf("FAIL: the log is empty or not terminated\n"); fails++; }
    else if (log.compare(0, 16, "Screamer Pocket ") != 0) { printf("FAIL: the log does not start with its header line\n"); fails++; }
    if (expect_trap) {
        char line[32];
        snprintf(line, sizeof line, "TRAP mcause=%08x", expect & 0x3FFFFFFFu);
        if (text.find(line) == std::string::npos) { printf("FAIL: no \"%s\" in the log\n", line); fails++; }
    } else {
        bool reported = text.find(": FAIL") != std::string::npos;
        if (reported != negative) { printf(negative ? "FAIL: the log reports no failed test\n" : "FAIL: the log reports a failed test\n"); fails++; }
        if (slot_line && text.find(slot_line) == std::string::npos) { printf("FAIL: no \"%s\" in the log\n", slot_line); fails++; }
        if (text.find("windows of 2048 bytes") == std::string::npos) { printf("FAIL: the log does not name the simulation windows\n"); fails++; }
        // the texture demo checks its triangles, the race (a track in slot 3) its whole picture
        const char *check = getenv("SIM_TRACK") ? "track check: ok" : "raster check: ok";
        if (!negative && text.find(check) == std::string::npos) { printf("FAIL: no \"%s\" in the log\n", check); fails++; }
    }
    return fails;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s firmware.bin outprefix [timeout frames] [expected SYS_SIM]\n", argv[0]); return 2; }
    int timeout_frames = argc > 3 ? atoi(argv[3]) : 80;
    uint32_t expect = argc > 4 ? uint32_t(strtoul(argv[4], nullptr, 16)) : 0x80001FFFu;
    bool expect_trap = (expect & 0xC0000000u) == 0xC0000000u;
    bool negative = !expect_trap && expect != 0x80001FFFu;

    std::vector<uint8_t> fw;
    {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fw.insert(fw.end(), buf, buf + n);
        fclose(f);
    }
    if (fw.empty() || fw.size() > 32768) { fprintf(stderr, "firmware size %zu is not 1..32768\n", fw.size()); return 2; }
    while (fw.size() % 4) fw.push_back(0);

    // program.bin next to firmware.bin (tools/fw.sh builds both)
    Sim s;
    std::vector<uint8_t> &prog = s.prog;
    {
        std::string path = argv[1];
        size_t slash = path.find_last_of("/\\");
        path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "program.bin";
        FILE *f = fopen(path.c_str(), "rb");
        if (f) {
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) prog.insert(prog.end(), buf, buf + n);
            fclose(f);
        }
        while (prog.size() % 4) prog.push_back(0);
        printf("%s: %zu bytes\n", path.c_str(), prog.size());
    }
    if (const char *tp = getenv("SIM_TRACK")) {         // slot 3, e.g. build/assets/.../track1.bin
        FILE *f = fopen(tp, "rb");
        if (f) {
            uint8_t buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.track.insert(s.track.end(), buf, buf + n);
            fclose(f);
        }
        while (s.track.size() % 4) s.track.push_back(0);
        printf("%s: %zu bytes (slot 3)\n", tp, s.track.size());
    }

    Vsoc_system &top = s.top;
    top.clk_74a = 0; top.clk_vid = 0; top.reset_n = 0; top.bridge_wr = 0; top.bridge_rd = 0; top.cont1_key = 0;
    top.ds_done = 0; top.ds_err = 0;
    top.eval();

    // as the APF does it: slot 0 (firmware), slot 1 (no debug.txt yet: 0xFF), then it waits
    // for the acknowledge (the SDRAM is ready); slots 2 and 3 only when the core asks
    int fails = 0;
    s.load(0, fw);
    s.load(0x00100000, std::vector<uint8_t>(4096, 0xFF));
    {
        // the core ignores the debug log's load (docs/soc.md), so it still holds what it held
        int loaded = 0;
        s.bridge_read(0x00100000);
        for (uint32_t a = 4; a <= 64; a += 4) loaded += s.bridge_read(0x00100000 + a) == 0xFFFFFFFFu;
        printf("APF load of the debug log slot: %s\n", loaded ? "taken (FAIL)" : "ignored");
        fails += loaded != 0;
    }
    uint64_t wait_from = s.now;
    while (!top.sdram_ready) s.rise_74();
    printf("SDRAM ready %.1f us after the first two slots\n", double(s.now - wait_from) / 1e6);
    s.rise_74(16);
    top.reset_n = 1; top.eval();

    std::vector<uint8_t> last;
    bool done, unasked;
    int frames = s.run(timeout_frames, last, done, unasked);
    fails += check_run(s, argv[2], frames, done, last, expect, timeout_frames,
                       expect_trap || negative ? nullptr : "SDRAM program: ok");
    if (!expect_trap && !negative && s.slot_reads < 2) {
        printf("FAIL: the core asked for %d data slot reads, expected program.bin's header and the file\n",
               s.slot_reads);
        fails++;
    }

    if (!expect_trap && !negative) {
        // a reset as when the firmware is reloaded from the menu (Reset Enter, the slot
        // load, Reset Exit), while buffer 1 is in front
        printf("==== reset while buffer 1 is in front, firmware loaded again ====\n");
        uint64_t limit = s.now + 4 * FRAME_PS;
        while (!(top.swap_req && top.front) && s.now < limit) s.step();
        if (!(top.swap_req && top.front)) { printf("FAIL: buffer 1 never in front with no swap pending\n"); fails++; }
        top.reset_n = 0; top.eval();
        s.rise_74(16);
        printf("in reset: swap request %d, buffer in front %d, SYS_SIM 0x%08x\n", top.swap_req, top.front, top.sim_status);
        if (top.swap_req || top.front || top.sim_status) { printf("FAIL: all three should be 0\n"); fails++; }
        s.load(0, fw);
        s.rise_74(16);
        top.reset_n = 1; top.eval();

        // and one in the middle of the memory test's SDRAM traffic
        s.run(timeout_frames, last, done, unasked, 1500000);
        printf("==== reset in the middle of the memory test, firmware loaded again ====\n");
        top.reset_n = 0; top.eval();
        s.load(0, fw);
        s.rise_74(16);
        top.reset_n = 1; top.eval();
        frames = s.run(timeout_frames, last, done, unasked);
        if (unasked) { printf("FAIL: a swap after the reset that the CPU did not ask for\n"); fails++; }
        else printf("after the reset: buffer 0 in front until the CPU's first swap\n");
        fails += check_run(s, std::string(argv[2]) + "_reset", frames, done, last, expect, timeout_frames,
                           "SDRAM program: ok");
    }

    if (fails) { printf("FAIL\n"); return 1; }
    printf(expect_trap ? "PASS: the trap was taken and reported on screen and in the log, slot size set\n"
           : negative ? "PASS: the memory test reports the built-in fault: exactly the expected tests fail\n"
                      : "PASS: firmware and memory test pass, test band matches, log readable over the bridge, "
                        "the SDRAM model without errors; the same after two resets, which swapped nothing by themselves\n");
    return 0;
}
