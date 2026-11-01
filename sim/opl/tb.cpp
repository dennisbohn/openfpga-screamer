// FM music testbench: opl.v with the real JTOPL chip. Checks
//  - the queue: a write is ignored while run is 0 and the chip is ready 128 of its clocks after
//    run; writes of one time are released at that time (not before) and 129 chip clocks apart,
//    a write that is late at once, the queue empties and is cleared by run = 0;
//  - the chip makes the tone it was told to, at the right pitch: the frequency of the output
//    frames (48 kHz, as audio.v takes them, `frame` strobes at its rate) against
//    fnum * 49716 / 2^(20 - block), which tests the chip's clock (3.579545 MHz from 74.25 MHz)
//    and the resampling together; the gain doubles the output; run = 0 silences it.
//
// Usage: Vopl     tools/sim.sh opl
// Prints PASS or FAIL and exits with 0 or 1.
//
// Vopl play <events> <seconds> <out.wav> [gain]: plays a song instead. <events> is what
// `fmdump <MUSIC.DAT> <song> <seconds> events` prints (tools/host/fmdump.c): a write's time in
// chip samples from the song's start, the register, the value. They are queued the way the
// firmware does, every 20 ms those due within 60 ms; the 48 kHz frames go to a 16-bit mono WAV
// file (the mixer takes them as they are, times the gain / 32, default 50).

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include "Vopl.h"
#include "verilated.h"

static Vopl *top;
static uint64_t cycles;
static uint32_t fr_acc;                     // 48 kHz from 74.25 MHz: 8 / 12375 a clock
static std::vector<int16_t> frames;         // the value at each frame strobe (the one before's)
static bool want_frames;
static int errors;

static void check(bool ok, const char *what)
{
    if(!ok) { printf("FAIL: %s\n", what); errors++; }
}

static void tick()
{
    fr_acc += 8;
    bool strobe = fr_acc >= 12375;
    if(strobe) fr_acc -= 12375;
    top->frame = strobe;
    top->clk = 0; top->eval();
    top->clk = 1; top->eval();
    if(strobe && want_frames) frames.push_back(top->fm);
    top->wr = 0;
    cycles++;
}

static void run(uint64_t n) { while(n--) tick(); }

static void write_reg(unsigned a, uint32_t d)
{
    top->wr = 1; top->wr_addr = a; top->wr_data = d;
    tick();
}

static void push(uint16_t when, unsigned reg, unsigned val)
{
    write_reg(1, (uint32_t)when << 16 | reg << 8 | val);
}

struct event { double due; unsigned reg, val; };

static int play(const char *evfile, double seconds, const char *wavfile, unsigned gain)
{
    std::vector<event> ev;
    FILE *f = fopen(evfile, "r");
    if(!f) { perror(evfile); return 2; }
    event e;
    while(fscanf(f, "%lf %x %x", &e.due, &e.reg, &e.val) == 3) ev.push_back(e);
    fclose(f);
    write_reg(4, gain);
    write_reg(0, 1);
    while(!top->ready) tick();
    run(100);
    uint32_t now32 = top->now, base = now32 + 2000;
    size_t next = 0;
    want_frames = true;
    const uint64_t end = cycles + (uint64_t)(seconds * 74250000.0), step = 1485000;     // 20 ms
    while(cycles < end) {
        now32 += (uint16_t)(top->now - (uint16_t)now32);
        while(next < ev.size() && top->fill < 208 &&
              (int32_t)(base + (uint32_t)(int32_t)llround(ev[next].due) - now32) <= 3000) {
            push((uint16_t)(base + (uint32_t)(int32_t)llround(ev[next].due)), ev[next].reg, ev[next].val);
            next++;
        }
        run(step);
    }
    double sum = 0;
    int peak = 0;
    for(int16_t v : frames) { sum += (double)v * v; if(abs(v) > peak) peak = abs(v); }
    printf("%zu of %zu writes, %zu frames: peak %d, rms %.0f\n", next, ev.size(), frames.size(), peak,
           frames.empty() ? 0.0 : sqrt(sum / (double)frames.size()));
    FILE *w = fopen(wavfile, "wb");
    if(!w) { perror(wavfile); return 2; }
    uint32_t bytes = (uint32_t)frames.size() * 2, hdr[11] = {
        0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00010001, 48000, 96000, 0x00100002, 0x61746164, bytes };
    fwrite(hdr, 4, 11, w);
    fwrite(frames.data(), 2, frames.size(), w);
    fclose(w);
    return 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vopl;
    top->reset = 1;
    run(4);
    top->reset = 0;
    run(10);

    if(argc > 4 && !strcmp(argv[1], "play"))
        return play(argv[2], atof(argv[3]), argv[4], argc > 5 ? (unsigned)atoi(argv[5]) : 50);

    // run = 0: a write is ignored, nothing ready
    push(0, 0x01, 0x20);
    run(4);
    check(top->fill == 0, "a write with run = 0 was taken");
    check(!top->ready, "the chip is ready before run");

    // run: ready after 128 chip clocks (2655 clocks)
    write_reg(0, 1);
    uint64_t t0 = cycles;
    while(!top->ready && cycles - t0 < 6000) tick();
    check(cycles - t0 > 2500 && cycles - t0 < 2800, "the chip is not ready 128 chip clocks after run");
    run(100);

    // times: three writes at now + 60, one more at the same time, one late (in the past)
    uint16_t base = top->now;
    uint16_t when = (uint16_t)(base + 60);
    push(when, 0x01, 0x20);
    push(when, 0x08, 0x00);
    push(when, 0xBD, 0xC0);
    push(base - 20, 0x04, 0x00);            // late: after the three, at once
    check(top->fill == 4, "the queue does not hold the four writes");
    uint64_t rel[4];
    int got = 0, fill = 4;
    bool early = false;
    for(uint64_t i = 0; i < 400000 && got < 4; i++) {
        tick();
        if(top->fill < fill) {
            fill = top->fill;
            rel[got++] = cycles;
            if(got == 1 && (int16_t)(top->now - when) < 0) early = true;
        }
    }
    check(got == 4, "not all four writes were taken");
    if(got == 4) {
        double dt1 = (double)(rel[1] - rel[0]), dt2 = (double)(rel[2] - rel[1]), dt3 = (double)(rel[3] - rel[2]);
        printf("releases: gaps %.0f %.0f %.0f clocks (129 chip clocks = %.0f)\n", dt1, dt2, dt3, 129 * 74250000.0 / 3579545);
        check(!early, "the first write was made before its time");
        check(dt1 > 2600 && dt1 < 2760 && dt2 > 2600 && dt2 < 2760 && dt3 > 2600 && dt3 < 2760,
              "the writes are not 129 chip clocks apart");
    }
    run(5000);
    check(top->fill == 0 && !top->busy, "the queue is not empty and idle at the end");

    // a note: channel 0, sine, 875 Hz (block 5, fnum 0x241)
    want_frames = true;
    uint16_t at = (uint16_t)(top->now + 100);
    push(at, 0x20, 0x21); push(at, 0x40, 0x10); push(at, 0x60, 0xF0); push(at, 0x80, 0x07);
    push(at, 0x23, 0x21); push(at, 0x43, 0x00); push(at, 0x63, 0xF0); push(at, 0x83, 0x07);
    push(at, 0xC0, 0x01);
    push(at, 0xA0, 0x41);
    push(at, 0xB0, 0x36);                   // key on, block 5, fnum high 2
    const int N = 14400 + 4800;             // 0.4 s of frames
    while((int)frames.size() < N) tick();
    // the frequency from the rising zero crossings after 0.1 s, with hysteresis
    double first = -1, last = -1, pmin = 1e9, pmax = 0;
    int crossings = 0, peak = 0;
    bool below = false;                     // armed by the first negative half wave
    for(int i = 4800; i < N; i++) {
        int v = frames[i];
        if(abs(v) > peak) peak = abs(v);
        if(below && v > 300) {
            below = false;
            // the crossing between i-1 and i, found at zero on the straight line (use the last
            // sample under 0 before)
            int j = i;
            while(j > 4800 && frames[j - 1] > 0) j--;
            double x = j - 1 + (double)(-frames[j - 1]) / (double)(frames[j] - frames[j - 1]);
            if(first < 0) first = x;
            else { double per = x - last; if(per < pmin) pmin = per; if(per > pmax) pmax = per; }
            last = x;
            crossings++;
        }
        if(v < -300) below = true;
    }
    double f = crossings > 1 ? (crossings - 1) * 48000.0 / (last - first) : 0;
    double expect = 577 * 49716.0 / 32768.0;
    printf("tone: peak %d, %d periods (%.3f..%.3f frames), %.2f Hz (chip: %.2f Hz)\n", peak, crossings, pmin, pmax, f, expect);
    check(peak > 2000, "the chip's tone is too quiet or missing");
    check(fabs(f - expect) / expect < 0.0005, "the pitch is not the chip's");

    // gain 64 instead of 32: twice the output
    write_reg(4, 64);
    run(60000);
    frames.clear();
    while(frames.size() < 2400) tick();
    int peak2 = 0;
    for(int v : frames) if(abs(v) > peak2) peak2 = abs(v);
    printf("gain 64: peak %d\n", peak2);
    check(peak2 > peak * 18 / 10 && peak2 < peak * 22 / 10, "the gain does not double the output");

    // run = 0 silences the chip and empties the queue
    push(top->now + 1000, 0x01, 0x20);
    check(top->fill == 1, "the write is not queued");
    write_reg(0, 0);
    run(30);
    check(top->fill == 0 && top->fm == 0, "run = 0 does not clear the queue and the output");
    frames.clear();
    while(frames.size() < 100) tick();
    int loud = 0;
    for(int v : frames) loud |= v != 0;
    check(!loud, "the output is not silent after run = 0");

    printf("%s\n", errors ? "FAIL" : "PASS");
    delete top;
    return errors ? 1 : 0;
}
