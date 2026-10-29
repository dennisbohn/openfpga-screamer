// audio testbench: the mixer (audio.v) against its C model firmware/mixer.c, frame by frame,
// as the I2S output carries it. Random voices: anywhere in the 64 MB, random ends, loops of
// random length and count (none, a few, for ever), steps up to 4 samples a frame, volumes and
// random bytes (sums of many loud voices clip); between passes random register writes as the
// CPU makes them: voices started (also again while they play), stopped, steps, volumes and ends
// changed. Lines come from a model of the SDRAM line port with random delays. Each frame the
// I2S bits (LRCK, the bit clock, the data) are decoded and compared with the model's frame of
// the pass before; no pass may be late.
//
// Usage: Vaudio [frames, default 20000] [seed, default 1]
//   tools/sim.sh audio 20000 1
// Prints PASS or FAIL (with the first differing frame) and exits with 0 or 1.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>
#include "Vaudio.h"
#include "verilated.h"
extern "C" {
#include "../../firmware/mixer.c"
}

static Vaudio *top;
static uint64_t cycles;
static uint32_t rng = 1;

static uint32_t rnd()
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static uint32_t rr(uint32_t n) { return rnd() % n; }

// the SDRAM: all 64 MB, random bytes, a quarter of them 0x80 (silence) in runs
static const uint32_t MEM = 1u << 26;
static std::vector<uint8_t> mem(MEM);
struct line_req { uint32_t addr; uint64_t at; int k; };
static std::deque<line_req> lines;

// the I2S decoder: bits on the rising edges of the bit clock, the half's 16 bits after its first
static int sclk_prev, lr_prev = -1, bitk;
static uint32_t word;
static int16_t got_l;
static std::vector<int16_t> dec_l, dec_r;   // decoded frames
static std::vector<int> dec_pass;           // passes done when the frame's left half began
static int passes, busy_prev;

static void tick()
{
    // the line port: ready two clocks of three; a line's words 4 to 40 clocks later, in two
    // clocks of three
    top->mem_ready = rr(3) != 0;
    bool ask = top->mem_valid && top->mem_ready;
    uint32_t ask_addr = (uint32_t)top->mem_addr << 5;
    top->mem_rsp_valid = 0;
    if (!lines.empty() && lines.front().at <= cycles && rr(3) != 0) {
        line_req &l = lines.front();
        uint32_t a = l.addr + 4u * (uint32_t)l.k;
        top->mem_rsp_valid = 1;
        top->mem_rsp_data = mem[a] | mem[a + 1] << 8 | mem[a + 2] << 16 | (uint32_t)mem[a + 3] << 24;
        if (++l.k == 8) lines.pop_front();
    }
    top->clk = 1;
    top->eval();
    if (ask) lines.push_back({ask_addr, cycles + 4 + rr(37), 0});
    cycles++;
    top->clk = 0;
    top->eval();

    if (busy_prev && !top->busy) passes++;
    busy_prev = top->busy;
    if (top->i2s_sclk && !sclk_prev) {
        int lr = top->i2s_lrck;
        if (lr != lr_prev) {
            bitk = 0;
            if (lr == 0) dec_pass.push_back(passes);
        } else bitk++;
        lr_prev = lr;
        if (bitk >= 1 && bitk <= 16) word = word << 1 | top->i2s_dac;
        if (bitk == 16) {
            if (lr == 0) got_l = (int16_t)word;
            else if (dec_pass.size() > dec_l.size()) {
                dec_l.push_back(got_l);
                dec_r.push_back((int16_t)word);
            }
        }
    }
    sclk_prev = top->i2s_sclk;
}

static struct mixer model;
static void reg_wr(uint32_t a, uint32_t d)
{
    top->wr = 1;
    top->wr_addr = a >> 2;
    top->wr_data = d;
    tick();
    top->wr = 0;
    mix_write(&model, a, d);
}

// a voice set up and started: a sample somewhere, its end, loop, step and volume
static void start_voice(int v)
{
    uint32_t addr = rnd() & (MEM - 1), len = 1 + rr(rr(4) ? 20000 : 200);
    uint32_t times = rr(4) == 0 ? 0 : rr(3) == 0 ? 255 : rr(6);
    uint32_t loop = rr(4) ? 1 + rr(len) : rnd() & 0xFFFFF;
    uint32_t step = rr(8) ? 0x1000 + rr(0x10000) : rnd() & 0x3FFFF;
    reg_wr(32 * v + 0, addr | (rnd() & 0xFC000000u));
    reg_wr(32 * v + 4, (addr + len) & (MEM - 1));
    reg_wr(32 * v + 8, times << 24 | loop | (rnd() & 0x00F00000u));
    reg_wr(32 * v + 12, step | (rnd() & 0xFFFC0000u));
    reg_wr(32 * v + 16, rr(3) ? rr(256) | rr(256) << 8 : rnd());
    reg_wr(0x400, 1u << v);
}

// the CPU's writes between two passes
static void writes()
{
    int n = rr(4) ? rr(3) : rr(12);
    for (int i = 0; i < n; i++) {
        int v = (int)rr(32);
        switch (rr(8)) {
        case 0: case 1: case 2: start_voice(v); break;
        case 3: reg_wr(0x404, rr(4) ? 1u << v : rnd()); break;
        case 4: reg_wr(32 * v + 12, 0x1000 + rr(0x20000)); break;
        case 5: reg_wr(32 * v + 16, rnd()); break;
        case 6: reg_wr(32 * v + 4, rnd()); break;
        default: reg_wr(0x400, 1u << v); break;                // again, from its first byte
        }
    }
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int frames = argc > 1 ? atoi(argv[1]) : 20000;
    rng = argc > 2 ? (uint32_t)atoi(argv[2]) * 2654435761u + 1 : 1;
    for (uint32_t a = 0; a < MEM; a += 4) {
        uint32_t r = rnd();
        for (int k = 0; k < 4; k++) mem[a + k] = (uint8_t)(r >> (8 * k));
    }
    for (int i = 0; i < 2000; i++) {                        // runs of silence
        uint32_t a = rnd() & (MEM - 1), n = rr(30000);
        for (uint32_t k = 0; k < n && a + k < MEM; k++) mem[a + k] = 0x80;
    }

    top = new Vaudio;
    top->clk = 0;
    top->reset = 1;
    top->wr = 0;
    top->eval();
    for (int i = 0; i < 8; i++) tick();
    top->reset = 0;
    mix_reset(&model);

    // pass k: the writes after pass k - 1, then the model's frame k; the hardware's pass k
    // starts with the next frame and is heard in the frame after
    std::vector<int16_t> exp_l, exp_r;
    int fails = 0, played = 0, fetched = 0;
    uint64_t limit = (uint64_t)(frames + 4) * 1600;
    while ((int)exp_l.size() < frames && cycles < limit) {
        writes();
        if (top->busy) {
            printf("FAIL: the writes ran into a pass (testbench timing)\n");
            return 1;
        }
        mix_frame(&model, mem.data());
        exp_l.push_back(model.out_l);
        exp_r.push_back(model.out_r);
        played += __builtin_popcount(model.playing);
        int p = passes;
        while (passes == p && cycles < limit) {
            fetched += top->mem_valid && top->mem_ready;
            tick();
        }
        if (top->late) {
            printf("FAIL: a pass was late (frame %zu)\n", exp_l.size());
            return 1;
        }
    }
    // the frames heard: frame j carries pass dec_pass[j] - 1
    size_t checked = 0;
    int clipped = 0;
    for (size_t j = 0; j < dec_l.size(); j++) {
        int p = dec_pass[j] - 1;
        if (p >= (int)exp_l.size()) break;
        int16_t el = p < 0 ? 0 : exp_l[p], er = p < 0 ? 0 : exp_r[p];
        clipped += el == 32767 || el == -32768;
        if (dec_l[j] != el || dec_r[j] != er) {
            if (fails < 5)
                printf("frame %zu (pass %d): hardware %d %d, model %d %d\n", j, p, dec_l[j], dec_r[j], el, er);
            fails++;
        }
        checked++;
    }
    printf("%zu frames checked (%zu passes), %d voice frames played, %d lines fetched, %d frames clipped left\n",
           checked, exp_l.size(), played, fetched, clipped);
    if (fails || checked + 2 < exp_l.size() || exp_l.size() < (size_t)frames) {
        printf("FAIL: %d frames differ\n", fails);
        return 1;
    }
    printf("PASS: the mixer matches its C model frame for frame\n");
    delete top;
    return 0;
}
