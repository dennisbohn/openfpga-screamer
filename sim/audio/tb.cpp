// Audio testbench: the mixer (audio.v) against its C model, firmware/mixer.c, frame by frame, as
// the I2S output carries it.
//
// Usage: Vaudio [frames, default 20000] [seed, default 1]
//   tools/sim.sh audio 20000 1
// Prints PASS or FAIL (with the first differing frame) and exits with 0 or 1.
//
// How it is driven
//   - Random voices: sample anywhere in the 64 MB, random ends, loops of random length and count
//     (none, a few, for ever), steps up to 4 samples a frame, random volumes and random bytes.
//     The sum of many loud voices clips, which the test covers.
//   - Between two passes of the mixer, random register writes as the CPU makes them: voices
//     started (also again while they play), stopped, and their steps, volumes and ends changed.
//   - The music on top: its ring anywhere in memory, random 16-bit frames; now and then it is
//     started from the start again, stopped, or its volume or ring is changed.
//   - Sample lines come from a model of the SDRAM line port with random delays.
//
// Checks
//   - Each frame of the I2S output (LRCK, the bit clock and the data are decoded) is compared with
//     the frame the model made in the pass before.
//   - After each pass the music's place is compared with the model's.
//   - No pass may be late.

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

// The SDRAM: all 64 MB, random bytes, a quarter of them 0x80 (silence) in runs.
// lines holds the line reads the mixer has started and not yet got all words for: the line's byte
// address, the clock from which its words may come, and the number of words sent so far.
static const uint32_t MEM = 1u << 26;
static std::vector<uint8_t> mem(MEM);
struct line_req { uint32_t addr; uint64_t at; int k; };
static std::deque<line_req> lines;

// The I2S decoder (in tick): a data bit is taken at each rising edge of the bit clock. The bit at
// the edge where LRCK changes is skipped, and the next 16 bits are the sample of that half.
// dec_l and dec_r are the decoded frames. passes counts the mixer passes done, and dec_pass
// remembers its value when a frame's left half began.
static int sclk_prev, lr_prev = -1, bitk;
static uint32_t word;
static int16_t got_l;
static std::vector<int16_t> dec_l, dec_r;
static std::vector<int> dec_pass;
static int passes, busy_prev;

// One clock: the model of the SDRAM line port, then the I2S decoder and the pass counter.
static void tick()
{
    // The line port: mem_ready is high in two clocks of three. The words of a line may start to
    // come 4 to 40 clocks after the request, and a word comes in two clocks of three.
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

// A write to the mixer's register at byte address a (voice registers at 32 * voice, the rest at
// 0x400 and up, see the header of audio.v). The model gets the same write.
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

// Sets up voice v and starts it: a sample somewhere, its end, loop (length and count), step and
// volume. The bits that the registers ignore get random values.
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

// The CPU's writes between two passes: usually 0 to 2, sometimes up to 11. Half of them start or
// restart a voice; the others stop voices or change a voice's step, volume or end. One time in 32
// the music is changed as well.
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
    if (rr(32) == 0) {                                          // the music
        switch (rr(8)) {
        case 0: reg_wr(0x414, rr(4) ? 0 : rnd() & ~1u); break;  // stopped
        case 1: case 2: reg_wr(0x414, rnd() | 1); break;        // from the ring's start
        case 3: case 4: case 5: reg_wr(0x410, rr(2) ? rnd() : rr(0x8000)); break;   // its volume
        default: reg_wr(0x40C, rnd()); reg_wr(0x414, 1); break; // another ring (set before a start)
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
    for (int i = 0; i < 2000; i++) {                        // runs of silence (0x80, the centre)
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
    reg_wr(0x40C, rnd());                                       // the music from the start
    reg_wr(0x410, 0x6000);
    reg_wr(0x414, 1);

    // pass k: the writes after pass k - 1, then the model's frame k; the hardware's pass k
    // starts with the next frame and is heard in the frame after
    // exp_l and exp_r are the model's frames. played, fetched and music only feed the statistics
    // line at the end: voice frames played, lines fetched, frames with the music. The limit is a
    // timeout in clocks (one frame is 1547 clocks).
    std::vector<int16_t> exp_l, exp_r;
    int fails = 0, played = 0, fetched = 0, music = 0;
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
        music += model.mu_on && !model.mu_fill;
        int p = passes;
        while (passes == p && cycles < limit) {
            fetched += top->mem_valid && top->mem_ready;
            tick();
        }
        // The pass is over: it must not have been late, and the music's place must be the model's.
        if (top->late) {
            printf("FAIL: a pass was late (frame %zu)\n", exp_l.size());
            return 1;
        }
        if (top->music != (model.mu_on << 16 | model.mu_idx)) {
            printf("FAIL: after pass %zu the music's place is 0x%05x, the model's 0x%05x\n", exp_l.size() - 1,
                   top->music, model.mu_on << 16 | model.mu_idx);
            return 1;
        }
    }
    // The frames heard: frame j carries pass dec_pass[j] - 1; a frame heard before the first pass is
    // silent. Compare the decoded I2S frames with the model's and count the left samples that clip.
    size_t checked = 0;
    int clipped = 0;
    for (size_t j = 0; j < dec_l.size(); j++) {
        int p = dec_pass[j] - 1;
        if (p >= (int)exp_l.size()) break;
        int16_t el = p < 0 ? 0 : exp_l[p], er = p < 0 ? 0 : exp_r[p];
        clipped += el == 32767 || el == -32768;
        if (dec_l[j] != el || dec_r[j] != er) {
            if (fails < 5)
                printf("frame %zu (pass %d): hardware %d %d, model %d %d\n", j, p, dec_l[j], dec_r[j],
                       el, er);
            fails++;
        }
        checked++;
    }
    printf("%zu frames checked (%zu passes), %d voice frames played, %d with the music, %d lines fetched, "
           "%d frames clipped left\n", checked, exp_l.size(), played, music, fetched, clipped);
    if (fails || checked + 2 < exp_l.size() || exp_l.size() < (size_t)frames) {
        printf("FAIL: %d frames differ\n", fails);
        return 1;
    }
    printf("PASS: the mixer matches its C model frame for frame\n");
    delete top;
    return 0;
}
