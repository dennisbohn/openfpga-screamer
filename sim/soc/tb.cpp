// soc testbench: the CPU system with its firmware and a model of the SDRAM, loaded the way
// the Pocket loads it, checked the way docs/soc.md and docs/memory.md describe.
//
// Usage: Vsoc_system <firmware.bin> <out prefix> [timeout in frames, default 80]
//                    [expected SYS_SIM in hex, default 80001FFF]
// Writes <prefix>.rgb (raw 320x200 RGB of the checked frame) and <prefix>_log.txt
// (the debug log as the APF would save it as debug.txt); <prefix>_reset.* the same for
// the run after the resets.
//
// Loading like the APF: the core held in reset, a bridge word at most every 88 clocks
// (io_bridge_peripheral.v), firmware.bin to bridge address 0, the debug log slot filled
// with 0xFF (debug.txt does not exist yet; the core must ignore it), the slot table (per
// slot of core/data.json its id and file size, deferload slots too), then the wait for the
// acknowledge core_top gives a slot write once the SDRAM is ready. Then reset is released.
// program.bin (next to firmware.bin) is deferload: the boot loader asks for it after the
// SDRAM tests (ds_read, core_bridge_cmd's target command 0x0180) and the testbench answers
// like the APF: done low, the words over the bridge, done high with the result (2 for a
// slot without a file). The boot loader checks program.bin and starts it. SIM_GAME names
// a folder with the game's original files: the testbench serves them as the slots
// core/data.json gives their names, and the program builds the track from them and drives
// the race instead of the list of missing files. The CD image SCREAM.gog (383 MB) is not read whole:
// the part asked for is read from the file and sent at the APF's measured rate while the
// simulation goes on, as the music streams it (firmware/music.c).
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
//
// SIM_WAV=<file> writes the I2S output of the whole run (decoded from LRCK, the bit clock and the
// data, 48 kHz stereo) to that WAV file (tools/simcheck.sh: build/sim/audio.wav).

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "Vsoc_system.h"

static const int W = 320, H = 200;
static const uint64_t HALF_74 = 6734, HALF_VID = 40690;    // ps
static const uint64_t FRAME_PS = 480ull * 512 * 2 * HALF_VID;   // 50 Hz
static const int BRIDGE_GAP = 88;                           // clocks per bridge word, APF worst case

static bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

// n bytes of a file from byte off
static bool read_part(const std::string &path, uint64_t off, uint64_t n, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.resize(n);
    bool ok = fseek(f, long(off), SEEK_SET) == 0 && fread(out.data(), 1, n, f) == n;
    fclose(f);
    return ok;
}

static uint64_t file_size(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n > 0 ? uint64_t(n) : 0;
}

// core/data.json's slots in its order (the order of the APF's slot table): id, file name,
// nonvolatile (the saves: no file in SIM_GAME, as on a Pocket that has none yet)
struct Slot { uint32_t id; std::string name; bool save; };
static std::vector<Slot> data_slots(const char *path) {
    std::vector<uint8_t> raw;
    std::vector<Slot> out;
    if (!read_file(path, raw)) return out;
    std::string s(raw.begin(), raw.end());
    size_t p = s.find("\"data_slots\"");
    while (p != std::string::npos && (p = s.find('{', p)) != std::string::npos) {
        size_t e = s.find('}', p);
        std::string o = s.substr(p, e - p);
        Slot slot{0, "", o.find("\"nonvolatile\"") != std::string::npos};
        size_t k = o.find("\"id\"");
        if (k != std::string::npos) slot.id = uint32_t(strtoul(o.c_str() + o.find(':', k) + 1, nullptr, 0));
        k = o.find("\"filename\"");
        if (k != std::string::npos) {
            size_t q = o.find('"', o.find(':', k)) + 1;
            slot.name = o.substr(q, o.find('"', q) - q);
        }
        out.push_back(slot);
        p = e;
    }
    return out;
}

// The I2S output as a WAV file: a bit on each rising edge of the bit clock, a half's 16 bits
// after its first (the one LRCK changed before), the left half while LRCK is low
struct Wav {
    FILE *f = nullptr;
    uint32_t frames = 0, word = 0;
    int sclk = 0, lrck = -1, k = 0;
    int16_t left = 0;
    bool have_left = false;
    void open(const char *path) {
        f = fopen(path, "wb");
        if (f) { uint8_t hdr[44] = { 0 }; fwrite(hdr, 1, 44, f); }
    }
    void clock(int s, int lr, int dac) {
        if (s && !sclk) {
            if (lr != lrck) k = 0; else k++;
            lrck = lr;
            if (k >= 1 && k <= 16) word = word << 1 | uint32_t(dac);
            if (k == 16) {
                if (!lr) { left = int16_t(word); have_left = true; }
                else if (have_left) {
                    int16_t fr[2] = { left, int16_t(word) };
                    fwrite(fr, 2, 2, f);
                    frames++;
                }
            }
        }
        sclk = s;
    }
    ~Wav() {
        if (!f) return;
        uint32_t bytes = frames * 4, v;
        uint16_t w;
        fseek(f, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
        fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
        w = 1; fwrite(&w, 2, 1, f); w = 2; fwrite(&w, 2, 1, f);
        v = 48000; fwrite(&v, 4, 1, f); v = 48000 * 4; fwrite(&v, 4, 1, f);
        w = 4; fwrite(&w, 2, 1, f); w = 16; fwrite(&w, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
        fclose(f);
        printf("audio: %u frames (%.2f s) of the I2S output in the WAV file\n", frames, frames / 48000.0);
    }
};
static Wav wav;

struct Sim {
    Vsoc_system top;
    uint64_t now = 0, next_74 = HALF_74, next_vid = HALF_VID;

    // one clock edge, whichever is next; returns 1 for a rising clk_74a edge, 2 for a rising clk_vid edge
    int step() {
        if (next_74 <= next_vid) {
            now = next_74; next_74 += HALF_74;
            top.clk_74a = !top.clk_74a; top.eval();
            if (wav.f && top.clk_74a) wav.clock(top.audio_sclk, top.audio_lrck, top.audio_dac);
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
    // one read transaction as io_bridge_peripheral.v does it, at most every BRIDGE_GAP clocks:
    // the address, 4 clocks, the answer is sampled, then the bridge_rd pulse. The answer
    // belongs to the previous read (from the SDRAM it may take most of the gap).
    uint32_t bridge_read(uint32_t addr) {
        rise_74(BRIDGE_GAP - 7);
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

    // the APF's side of a data slot read the core asked for: the file of slot id, program.bin
    // (slot 2) or one of the game's files (sent faster than the APF would, 2 MB take long).
    // Result 2 (error or out of range) for a slot without a file and for a read past the
    // file's end, the strict reading of the APF's docs; length 0xFFFFFFFF is the rest of the
    // file, its last word filled up with zeros. The clocks it takes do not count as frames
    // of run().
    std::map<uint32_t, std::vector<uint8_t>> files;     // by slot id
    bool race = false;                                  // every game file is there
    int slot_reads = 0;
    uint32_t keys = 0;                                  // SIM_KEYS, held from the program's first read

    // The CD image: read from the file where asked, its words over the bridge at the APF's
    // measured rate (1.3 MB/s: one every 220 clocks) while run() goes on (cd_tick); done goes
    // high after the last. A read still running when run() ends is finished at once (cd_finish).
    static const int CD_GAP = 220;
    std::string cd_path;
    uint32_t cd_id = 0, cd_addr = 0;
    uint64_t cd_size = 0;
    std::vector<uint8_t> cd_part;
    size_t cd_k = 0;
    int cd_wait = 0, cd_state = 0;                      // 0 none, 1 sending, 2 sent, 3 done: until ds_read falls
    bool cd_wr = false;                                 // a word is on the bridge for the next edge
    void cd_tick() {
        if (cd_wr) { top.bridge_wr = 0; top.eval(); cd_wr = false; }
        if (cd_state == 1 && --cd_wait <= 0) {
            const uint8_t *b = &cd_part[cd_k];
            top.bridge_addr = cd_addr + uint32_t(cd_k);
            top.bridge_wr_data = uint32_t(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3];
            top.bridge_wr = 1; top.eval();
            cd_wr = true;
            cd_k += 4;
            cd_wait = cd_k < cd_part.size() ? CD_GAP : 9;
            if (cd_k >= cd_part.size()) cd_state = 2;
        } else if (cd_state == 2 && --cd_wait <= 0) {
            top.ds_err = 0; top.ds_done = 1; top.eval();
            cd_state = 3;
        } else if (cd_state == 3 && !top.ds_read) {
            cd_state = 0;
        }
    }
    void cd_finish() {
        if (cd_wr) { rise_74(); top.bridge_wr = 0; top.eval(); cd_wr = false; }
        if (cd_state == 1) load(cd_addr + uint32_t(cd_k), std::vector<uint8_t>(cd_part.begin() + cd_k, cd_part.end()), 12);
        if (cd_state == 1 || cd_state == 2) { rise_74(8); top.ds_err = 0; top.ds_done = 1; top.eval(); }
        if (cd_state) while (top.ds_read) rise_74();
        cd_state = 0;
    }

    void serve_read() {
        top.ds_done = 0; top.eval();
        rise_74(8);
        uint32_t id = top.ds_id, off = top.ds_offset, addr = top.ds_addr, len = top.ds_length, err = 2;
        if (id >= 3) top.cont1_key = keys;              // the program runs: the boot loader is past its keys
        if (id == cd_id && cd_size) {
            uint64_t end = len == 0xFFFFFFFFu ? cd_size : uint64_t(off) + len;
            if (off < cd_size && end <= cd_size && read_part(cd_path, off, end - off, cd_part)) {
                while (cd_part.size() % 4) cd_part.push_back(0);
                printf("data slot read %u: %u bytes from %u to 0x%08x, sent while the simulation runs\n", id, len, off, addr);
                slot_reads++;
                cd_addr = addr;
                cd_k = 0;
                cd_wait = CD_GAP;
                cd_state = 1;
                return;
            }
        }
        auto it = files.find(id);
        if (it != files.end()) {
            const std::vector<uint8_t> &f = it->second;
            uint64_t end = len == 0xFFFFFFFFu ? f.size() : uint64_t(off) + len;
            if (off < f.size() && end <= f.size()) {
                std::vector<uint8_t> part(f.begin() + off, f.begin() + end);
                while (part.size() % 4) part.push_back(0);
                load(addr, part, id == 2 ? BRIDGE_GAP : 12);
                err = 0;
            }
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
            if (e == 1 && cd_state) cd_tick();
            else if (e == 1 && top.ds_read) serve_read();
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
        cd_finish();
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

    // the last whole frame: the test band (the boot loader's screen), the white title bar of the
    // list of the game's files (the program's screen without them, firmware/files.c: its right
    // end, where there is no text), or the trap handler's box (rows 18..24 in COL_ALERT; text
    // row 22 is empty); the race (the game's files) draws no band
    if (last.size() == size_t(W * H * 3)) {
        bool race = s.race && !expect_trap;
        auto wrong = [&](int y0, int x0, int x1, bool band, uint32_t fixed) {
            int bad = 0;
            for (int y = y0; y < y0 + 8; y++)
                for (int x = x0; x < x1; x++) {
                    uint32_t want = band ? pal_formula(x) : fixed;
                    const uint8_t *p = &last[(y * W + x) * 3];
                    uint32_t got = uint32_t(p[0]) << 16 | p[1] << 8 | p[2];
                    if (got != want) { if (bad < 5) printf("pixel %d,%d: %06x, expected %06x\n", x, y, got, want); bad++; }
                }
            return bad;
        };
        int bad = 0, n = 8 * 256;
        const char *what = "test band";
        if (expect_trap) {
            what = "trap box";
            n = 8 * W;
            bad = wrong(22 * 8, 0, W, false, pal_formula(230));
        } else if (!race) {
            int band = wrong(0, 0, 256, true, 0), bar = wrong(0, 256, W, false, 0xFFFFFF);
            if (bar == 0) { what = "title bar of the list of files"; n = 8 * (W - 256); }
            else bad = band;
        }
        printf("%s: %s\n", what,
               race ? "none in the race, not checked"
               : bad ? (std::to_string(bad) + " of " + std::to_string(n) + " pixels wrong").c_str()
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
    // CHOICE.DAT (the game's files there: the program wrote the game's defaults at boot), as
    // the APF saves it from the SDRAM: its size in the slot table, its first words (car 0,
    // track 0, the name GRA)
    if (s.race && !expect_trap) {
        printf("CHOICE.DAT slot size in the slot table: %u\n", top.choice_slot_size);
        if (top.choice_slot_size != 1120) { printf("FAIL: expected 1120\n"); fails++; }
        static const uint32_t want[3] = { 0, 0, 0x47524100u };
        s.bridge_read(0x13F00000);
        int bad = 0;
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t w = s.bridge_read(0x13F00004 + 4 * k);
            if (w != want[k]) { printf("CHOICE.DAT word %u through the bridge: %08x, expected %08x\n", k, w, want[k]); bad++; }
        }
        printf("CHOICE.DAT through the bridge from the SDRAM: %s\n", bad ? "FAIL" : "as written");
        fails += bad != 0;
    }
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
        // the race (the game's files) checks its whole picture, without them the program says
        // which files it misses
        const char *check = s.race ? "track check: ok" : "game files: ";
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
    // SIM_QUICK (tools/simcheck.sh): one run, without the two resets, about half the time
    const char *q = getenv("SIM_QUICK");
    bool quick = q && *q && *q != '0';

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
    {
        std::string path = argv[1];
        size_t slash = path.find_last_of("/\\");
        path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "program.bin";
        std::vector<uint8_t> &prog = s.files[2];
        read_file(path, prog);
        while (prog.size() % 4) prog.push_back(0);
        printf("%s: %zu bytes\n", path.c_str(), prog.size());
    }
    // the game's files from SIM_GAME by the names core/data.json gives the slots above 2
    std::vector<Slot> slots = data_slots("core/data.json");
    if (slots.empty()) { fprintf(stderr, "cannot read the slots of core/data.json\n"); return 2; }
    if (const char *game = getenv("SIM_GAME")) {
        int found = 0, wanted = 0;
        for (const Slot &slot : slots) {
            if (slot.id <= 2 || slot.save) continue;
            wanted++;
            std::string path = std::string(game) + "/" + slot.name;
            if (slot.name.size() > 4 && slot.name.compare(slot.name.size() - 4, 4, ".gog") == 0) {
                s.cd_size = file_size(path);            // the CD image: read where asked
                if (s.cd_size) {
                    s.cd_path = path;
                    s.cd_id = slot.id;
                    found++;
                    printf("%s: %llu bytes (slot %u, read where asked)\n", path.c_str(),
                           (unsigned long long)s.cd_size, slot.id);
                } else printf("%s: missing (slot %u)\n", path.c_str(), slot.id);
                continue;
            }
            std::vector<uint8_t> f;
            if (read_file(path, f) && !f.empty()) {
                s.files[slot.id] = f;
                found++;
                printf("%s/%s: %zu bytes (slot %u)\n", game, slot.name.c_str(), f.size(), slot.id);
            } else printf("%s/%s: missing (slot %u)\n", game, slot.name.c_str(), slot.id);
        }
        s.race = wanted > 0 && found == wanted;
    }
    // SIM_IMAGE (tools/simcheck.sh): the finished track image as data slot 30, which
    // core/data.json does not have; the firmware takes it instead of building the image
    if (const char *img = getenv("SIM_IMAGE")) {
        std::vector<uint8_t> f;
        if (*img && read_file(img, f) && !f.empty()) {
            s.files[30] = f;
            printf("%s: %zu bytes (slot 30, the track image)\n", img, f.size());
        }
    }

    // SIM_KEYS (tools/sim.sh): these buttons (hex, cont1_key's bits as firmware/hw.h names
    // them) held from the program's first read of a game file on (the boot loader reads the
    // buttons at its start: Select there asks for the full memory test); none without it
    if (const char *k = getenv("SIM_KEYS")) s.keys = uint32_t(strtoul(k, nullptr, 16));
    if (s.keys) printf("buttons held: 0x%04x, from the program's first game file read\n", s.keys);
    if (const char *w = getenv("SIM_WAV")) if (*w) wav.open(w);

    Vsoc_system &top = s.top;
    top.clk_74a = 0; top.clk_vid = 0; top.reset_n = 0; top.bridge_wr = 0; top.bridge_rd = 0; top.cont1_key = 0;
    top.ds_done = 0; top.ds_err = 0;
    top.eval();

    // as the APF does it: slot 0 (firmware), slot 1 (no debug.txt yet: 0xFF), then it waits
    // for the acknowledge (the SDRAM is ready); slots 2 and 3 only when the core asks
    int fails = 0;
    s.load(0, fw);
    s.load(0x00100000, std::vector<uint8_t>(4096, 0xFF));
    // the slot table as the APF writes it when it loads the core: per slot of core/data.json
    // its id and its file's size (none for the debug log: no debug.txt yet)
    for (size_t k = 0; k < slots.size() && k < 32; k++) {
        uint32_t id = slots[k].id;
        auto it = s.files.find(id);
        uint32_t size = id == 0 ? uint32_t(fw.size()) : id == s.cd_id && s.cd_size ? uint32_t(s.cd_size)
                        : it != s.files.end() ? uint32_t(it->second.size()) : 0;
        s.bridge_write(0xF8002000u + 8 * uint32_t(k), id);
        s.bridge_write(0xF8002004u + 8 * uint32_t(k), size);
    }
    size_t extra = slots.size();
    if (s.files.count(30) && extra < 32) {
        s.bridge_write(0xF8002000u + 8 * uint32_t(extra), 30);
        s.bridge_write(0xF8002004u + 8 * uint32_t(extra), uint32_t(s.files[30].size()));
        extra++;
    }
    // SIM_RACE_FRAMES=n (tools/sim.sh): slot 29, which core/data.json does not have either, with
    // the size n: the firmware runs the start lights' countdown at once and checks and reports
    // the n-th frame after the green light instead of the third (firmware/main.c)
    if (const char *r = getenv("SIM_RACE_FRAMES")) {
        uint32_t n = uint32_t(strtoul(r, nullptr, 10));
        if (n && extra < 32) {
            s.bridge_write(0xF8002000u + 8 * uint32_t(extra), 29);
            s.bridge_write(0xF8002004u + 8 * uint32_t(extra), n);
            printf("race: the frame %u after the green light (slot 29)\n", n);
            extra++;
        }
    }
    // SIM_TRACK=n (tools/sim.sh): slot 28 with the size n, the track the firmware races (none:
    // track 1; the simulation has no track screen)
    if (const char *t = getenv("SIM_TRACK")) {
        uint32_t n = uint32_t(strtoul(t, nullptr, 10));
        if (n && extra < 32) {
            s.bridge_write(0xF8002000u + 8 * uint32_t(extra), 28);
            s.bridge_write(0xF8002004u + 8 * uint32_t(extra), n);
            printf("track %u (slot 28)\n", n);
            extra++;
        }
    }
    // SIM_MENU=1 or 2 (tools/sim.sh): slot 27 with that size, the track screen (1) or the car
    // screen (2) instead of the race, its third frame checked against the C models
    if (const char *m = getenv("SIM_MENU")) {
        uint32_t n = uint32_t(strtoul(m, nullptr, 10));
        if (n && extra < 32) {
            s.bridge_write(0xF8002000u + 8 * uint32_t(extra), 27);
            s.bridge_write(0xF8002004u + 8 * uint32_t(extra), n);
            printf("the %s screen (slot 27)\n", n == 2 ? "car" : "track");
        }
    }
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

    if (!expect_trap && !negative && !quick) {
        // a reset as when the firmware is reloaded from the menu (Reset Enter, the slot
        // load, Reset Exit), while buffer 1 is in front
        printf("==== reset while buffer 1 is in front, firmware loaded again ====\n");
        uint64_t limit = s.now + 4 * FRAME_PS;
        while (!(top.swap_req && top.front) && s.now < limit) s.step();
        if (!(top.swap_req && top.front)) { printf("FAIL: buffer 1 never in front with no swap pending\n"); fails++; }
        top.reset_n = 0; top.cont1_key = 0; top.eval();
        s.rise_74(16);
        printf("in reset: swap request %d, buffer in front %d, SYS_SIM 0x%08x\n", top.swap_req, top.front, top.sim_status);
        if (top.swap_req || top.front || top.sim_status) { printf("FAIL: all three should be 0\n"); fails++; }
        s.load(0, fw);
        s.rise_74(16);
        top.reset_n = 1; top.eval();

        // and one in the middle of the memory test's SDRAM traffic
        s.run(timeout_frames, last, done, unasked, 1500000);
        printf("==== reset in the middle of the memory test, firmware loaded again ====\n");
        top.reset_n = 0; top.cont1_key = 0; top.eval();
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
           : quick ? "PASS (quick, no resets): firmware and memory test pass, test band matches, log readable "
                     "over the bridge, the SDRAM model without errors\n"
                      : "PASS: firmware and memory test pass, test band matches, log readable over the bridge, "
                        "the SDRAM model without errors; the same after two resets, which swapped nothing by themselves\n");
    return 0;
}
