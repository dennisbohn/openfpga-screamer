/*
 * Memory test of step 2 (docs/speicher.md). One set of tests for all three memories,
 * given their base addresses, size and die size; the SDRAM gets a few of its own: the
 * copy the APF loaded, the clock phase window, code, refresh and the write status.
 *
 * In the simulation (SYS_FLAGS) only the window sizes shrink: the first and the last
 * window of each memory and of each PSRAM die are tested, so loop ends, the die switch and
 * the left-out word run too. The SDRAM patterns go through the data cache (invalidated
 * before each check), everything else uses uncached addresses. Each error is logged with
 * the value read and two uncached re-reads: the same wrong value three times points to a
 * write or address fault, changing values to read timing.
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "memtest.h"

#define V32(a)          (*(volatile uint32_t *)(uintptr_t)(a))
#define V16(a)          (*(volatile uint16_t *)(uintptr_t)(a))
#define V8(a)           (*(volatile uint8_t *)(uintptr_t)(a))
#define VS16(a)         (*(volatile int16_t *)(uintptr_t)(a))
#define VS8(a)          (*(volatile int8_t *)(uintptr_t)(a))

/* SYS_SIM bits, counted from MT_BIT_FIRST */
enum {
    B_SD_READY, B_SD_SLOT, B_SD_PHASE, B_SD_DATA, B_SD_BYTES, B_SD_ADDR, B_SD_PATTERN,
    B_SD_CODE, B_SD_REFRESH, B_SD_STATUS,
    B_SR_DATA, B_SR_BYTES, B_SR_ADDR, B_SR_PATTERN,
    B_PS_DATA, B_PS_BYTES, B_PS_ADDR, B_PS_PATTERN,
};

enum { M_SDRAM, M_SRAM, M_PSRAM, M_N };

struct memory {
    const char *name;
    uint32_t base;              /* patterns are written and read here */
    uint32_t ubase;             /* the uncached view */
    uint32_t size;
    uint32_t die;               /* the windows repeat per die */
    uint8_t first_bit;          /* data bus, bytes, address and patterns follow */
    uint8_t cached;             /* invalidate the data cache before checking */
};

static const struct memory mems[M_N] = {
    { "SDRAM", SDRAM_BASE, SDRAM_UBASE, SDRAM_SIZE, SDRAM_SIZE, B_SD_DATA, 1 },
    { "SRAM", SRAM_BASE, SRAM_BASE, SRAM_SIZE, SRAM_SIZE, B_SR_DATA, 0 },
    { "cram0", PSRAM_BASE, PSRAM_BASE, PSRAM_SIZE, PSRAM_SIZE / 2, B_PS_DATA, 0 },
};

#define SIM_WINDOW      2048    /* bytes per window in the simulation */
#define MAX_LOGGED      8       /* error lines per memory */
#define PHASE_STEPS_MAX 80      /* 80 steps of 168 ps = one clock period */

static uint32_t results;        /* passed tests, bit numbers as in the enum */
static uint32_t errors[M_N];
static int sim;
static int small;               /* windows instead of the whole memory */
static const char *test;
static void (*progress)(const char *what, uint32_t addr);

/* for the screen */
static int phase_first = -1, phase_len, phase_centre;
static uint32_t refresh_per_s, line_hit, line_miss, read_mbs, write_mbs;
static uint8_t slot_state;      /* 0 not run, 1 ok, 2 failed, 3 skipped */
static char phase_map[PHASE_STEPS_MAX + 1];

static int skip(const struct memory *m, uint32_t off)
{
    /* PSRAM: the word holding halfword 3FFFFFh of each die (software access) */
    return m == &mems[M_PSRAM] && (off & (m->die - 1)) == m->die - 4;
}

static void fail(const struct memory *m, uint32_t off, uint32_t want, uint32_t got)
{
    if(errors[m - mems]++ < MAX_LOGGED) {
        uint32_t u = m->ubase + (off & ~3u);
        uint32_t r1 = V32(u), r2 = V32(u);
        dlog_str("  ");
        dlog_str(test);
        dlog_str(" @");
        dlog_hex(m->base + off);
        dlog_str(" want ");
        dlog_hex(want);
        dlog_str(" got ");
        dlog_hex(got);
        dlog_char(' ');
        dlog_hex(r1);
        dlog_char(' ');
        dlog_hex(r2);
        dlog_char('\n');
    }
}

static uint32_t begin(const struct memory *m, const char *name)
{
    test = name;
    progress(name, m->base);
    return errors[m - mems];
}

static void end(const struct memory *m, int bit, uint32_t before, uint32_t t0)
{
    uint32_t n = errors[m - mems] - before;
    if(!n) results |= 1u << bit;
    dlog_str(m->name);
    dlog_char(' ');
    dlog_str(test);
    if(n) {
        dlog_str(": FAIL, errors ");
        dlog_dec((int32_t)n);
    } else {
        dlog_str(": ok");
    }
    dlog_str(", cycles ");
    dlog_dec((int32_t)(rdcycle() - t0));
    dlog_char('\n');
}

/* Data bus: a walking one and a walking zero on one word */
static void t_databus(const struct memory *m)
{
    uint32_t t0 = rdcycle(), before = begin(m, "data bus"), off = 0x40;
    for(int b = 0; b < 32; b++) {
        uint32_t v = 1u << b, g;
        V32(m->ubase + off) = v;
        if((g = V32(m->ubase + off)) != v) fail(m, off, v, g);
        V32(m->ubase + off) = ~v;
        if((g = V32(m->ubase + off)) != ~v) fail(m, off, ~v, g);
    }
    end(m, m->first_bit, before, t0);
}

/* Byte lanes: one partial store into a fresh background word at a time, then sub-word
   reads with sign and zero extension */
static void t_bytes(const struct memory *m)
{
    static const uint8_t stores[6] = { 0, 1, 2, 3, 0x10, 0x12 };   /* 0x10: halfword */
    uint32_t t0 = rdcycle(), before = begin(m, "bytes"), off = 0x80, a = m->ubase + off;
    for(int inv = 0; inv < 2; inv++)
        for(int k = 0; k < 6; k++) {
            uint32_t bg = inv ? 0xA55A3CC3u : 0x5AA5C33Cu, o = stores[k] & 3, sh = 8 * o, want, g;
            V32(a) = bg;
            if(stores[k] & 0x10) {
                uint32_t v = 0x1234u ^ (uint32_t)k;
                V16(a + o) = (uint16_t)v;
                want = (bg & ~(0xFFFFu << sh)) | v << sh;
            } else {
                uint32_t v = 0x5Eu ^ (uint32_t)k;
                V8(a + o) = (uint8_t)v;
                want = (bg & ~(0xFFu << sh)) | v << sh;
            }
            if((g = V32(a)) != want) fail(m, off, want, g);
        }
    const uint32_t word = 0x8899AABBu;
    V32(a) = word;
    for(uint32_t o = 0; o < 4; o++) {
        uint32_t want = (word >> 8 * o) & 0xFF;
        uint32_t ub = V8(a + o);
        int32_t sb = VS8(a + o);
        if(ub != want) fail(m, off + o, want, ub);
        if(sb != (int8_t)want) fail(m, off + o, (uint32_t)(int8_t)want, (uint32_t)sb);
        if(!(o & 1)) {
            uint32_t wanth = (word >> 8 * o) & 0xFFFF;
            uint32_t uh = V16(a + o);
            int32_t shw = VS16(a + o);
            if(uh != wanth) fail(m, off + o, wanth, uh);
            if(shw != (int16_t)wanth) fail(m, off + o, (uint32_t)(int16_t)wanth, (uint32_t)shw);
        }
    }
    end(m, m->first_bit + 1, before, t0);
}

/* Address bus: own values at 0 and every power of two below the size, all read back
   after all are written (finds address and bank lines that stick or join) */
static uint32_t addr_value(uint32_t off)
{
    return ~off ^ 0x13579BDFu;
}

static void t_address(const struct memory *m)
{
    uint32_t t0 = rdcycle(), before = begin(m, "address");
    for(uint32_t o = 0; o < m->size; o = o ? o << 1 : 4)
        if(!skip(m, o)) V32(m->ubase + o) = addr_value(o);
    for(uint32_t o = 0; o < m->size; o = o ? o << 1 : 4) {
        uint32_t g;
        if(!skip(m, o) && (g = V32(m->ubase + o)) != addr_value(o)) fail(m, o, addr_value(o), g);
    }
    end(m, m->first_bit + 2, before, t0);
}

/* Patterns over the whole memory (in the simulation: the first and last window of each
   die): address, inverted address, pseudo-random, and two that switch all 16 data lines
   between halfwords */
static uint32_t xs;

#define XORSHIFT(x)     ((x) ^= (x) << 13, (x) ^= (x) >> 17, (x) ^= (x) << 5, (x))

static uint32_t next_random(void)
{
    return XORSHIFT(xs);
}

/* One loop per pattern, without calls inside: a pass over the SDRAM is 16M words */
#define PATTERN_LOOPS(LOOP)                                 \
    switch(kind) {                                          \
    case 0: LOOP(o ^ 0x0F1E2D3Cu); break;                   \
    case 1: LOOP(~o ^ 0x0F1E2D3Cu); break;                  \
    case 2: LOOP(XORSHIFT(x)); break;                       \
    case 3: LOOP(0xFFFF0000u); break;                       \
    default: LOOP(0xAAAA5555u); break;                      \
    }
#define FILL(v)     for(; o < end; o += 4) V32(base + o) = (v)
#define CHECK(v)    for(; o < end; o += 4) {                \
                        uint32_t want = (v), g = V32(base + o); \
                        if(__builtin_expect(g != want, 0))  \
                            fail(m, o, want, g);            \
                    }

static void fill(const struct memory *m, int kind, uint32_t o, uint32_t end)
{
    uint32_t base = m->base, x = xs;
    PATTERN_LOOPS(FILL)
    xs = x;
}

static void check(const struct memory *m, int kind, uint32_t o, uint32_t end)
{
    uint32_t base = m->base, x = xs;
    PATTERN_LOOPS(CHECK)
    xs = x;
}

static int windows(const struct memory *m, uint32_t w[][2])
{
    int n = 0;
    for(uint32_t d = 0; d < m->size; d += m->die) {
        if(small) {
            w[n][0] = d;
            w[n++][1] = d + SIM_WINDOW;
            w[n][0] = d + m->die - SIM_WINDOW;
            w[n++][1] = d + m->die;
        } else {
            w[n][0] = d;
            w[n++][1] = d + m->die;
        }
    }
    return n;
}

/* Write or check one pattern over the windows, 64 KB at a time between progress reports.
   A window that ends with a die ends before the PSRAM word skip() leaves out. */
static void pass(const struct memory *m, int kind, int checking, uint32_t w[][2], int nw)
{
    for(int i = 0; i < nw; i++) {
        uint32_t end = w[i][1];
        if(skip(m, end - 4)) end -= 4;
        xs = 0x9E3779B9u ^ w[i][0] ^ (uint32_t)kind;
        for(uint32_t o = w[i][0]; o < end; o += 0x10000) {
            uint32_t e = end - o > 0x10000 ? o + 0x10000 : end;
            progress(test, m->base + o);
            if(checking) check(m, kind, o, e);
            else fill(m, kind, o, e);
        }
    }
}

static void t_patterns(const struct memory *m)
{
    uint32_t w[4][2], t0 = rdcycle(), before = begin(m, "patterns");
    int nw = windows(m, w);
    for(int kind = 0; kind < 5; kind++) {
        pass(m, kind, 0, w, nw);
        if(m->cached) dcache_invalidate();
        pass(m, kind, 1, w, nw);
    }
    if(m->cached) {
        /* lines in a scrambled order (row misses right after writes), then stores read
           back at once through the uncached view */
        uint32_t lines = (small ? SIM_WINDOW : 1u << 20) / 32;
        for(int check = 0; check < 2; check++) {
            for(uint32_t i = 0; i < lines; i++) {
                uint32_t line = (i * (check ? 0x85EBCA77u : 0x9E3779B1u)) & (lines - 1);
                for(uint32_t o = line * 32; o < line * 32 + 32; o += 4) {
                    uint32_t v = o * 0x2545F491u, g;
                    if(!check) V32(m->base + o) = v;
                    else if((g = V32(m->base + o)) != v) fail(m, o, v, g);
                }
            }
            dcache_invalidate();
        }
        xs = 0x2468ACE1u;
        for(int i = 0; i < 256; i++) {
            uint32_t o = next_random() & (m->size - 32) & ~31u, v = xs ^ 0x55AA55AAu, g;
            uint32_t old = V32(m->ubase + o);   /* put back: data slots loaded into the SDRAM stay */
            V32(m->base + o) = v;
            if((g = V32(m->ubase + o)) != v) fail(m, o, v, g);
            V32(m->base + o) = old;
        }
    }
    end(m, m->first_bit + 3, before, t0);
}

static void t_memory(const struct memory *m)
{
    dlog_str(m->name);
    dlog_str(sim ? ": simulation, windows of 2048 bytes at the start and end of each die\n"
                 : small ? ": quick check, windows of 2048 bytes at the start and end of each die"
                           " (hold Select at start for the whole memory)\n"
                 : ": the whole memory\n");
    t_databus(m);
    if(m == &mems[M_PSRAM] && errors[M_PSRAM] >= 64) {
        dlog_str("cram0 does not answer (deep power-down left by another core?)\n");
        return;
    }
    t_bytes(m);
    t_address(m);
    t_patterns(m);
}

/*
 * SDRAM only
 */
static const struct memory *const sd = &mems[M_SDRAM];

static int sdram_reinit(void)
{
    MEM_PHASE = PHASE_REINIT;
    for(int i = 0; i < 1000 && (MEM_STATUS & MEM_READY); i++)
        ;
    uint32_t t0 = rdcycle();
    while(!(MEM_STATUS & MEM_READY))
        if(rdcycle() - t0 > CPU_HZ / 10) return 0;
    return 1;
}

static void phase_step(int later)
{
    MEM_PHASE = PHASE_STEP | (later ? PHASE_LATER : 0);
    while(MEM_PHASE & PHASE_BUSY)
        ;
}

/* The copy of firmware.bin the APF loaded into the SDRAM (data slot 2), if it did so in
   this run: a reload from the menu writes only the reloaded slot */
static void t_slot(uint32_t boot_crc)
{
    extern const uint8_t __image_start[], __image_end[];
    uint32_t size = (uint32_t)(__image_end - __image_start), words = MEM_BRIDGE_WORDS;
    uint32_t t0 = rdcycle(), before = begin(sd, "slot");
    if(words * 4 < size) {
        dlog_str("SDRAM slot: skipped (not loaded in this run), bridge words ");
        dlog_dec((int32_t)words);
        dlog_char('\n');
        results |= 1u << B_SD_SLOT;
        slot_state = 3;
        return;
    }
    uint32_t c = crc32((const uint8_t *)SDRAM_BASE, (const uint8_t *)SDRAM_BASE + size);
    if(c != boot_crc) fail(sd, 0, boot_crc, c);
    if(MEM_STATUS & (MEM_OVERFLOW | MEM_DROPPED)) fail(sd, 0, 0, MEM_STATUS);
    end(sd, B_SD_SLOT, before, t0);
    slot_state = errors[M_SDRAM] == before ? 1 : 2;
}

/* A short hard check for the phase sweep: pseudo-random words through the cache, words
   switching all data lines through the uncached view */
static int quick(uint32_t seed)
{
    uint32_t n = sim ? 512 : 4096, b = 0x10000, bad = 0;
    dcache_invalidate();
    xs = seed;
    for(uint32_t o = 0; o < n; o += 4) V32(SDRAM_BASE + b + o) = next_random();
    dcache_invalidate();
    xs = seed;
    for(uint32_t o = 0; o < n; o += 4) bad += V32(SDRAM_BASE + b + o) != next_random();
    for(uint32_t o = 0; o < 64; o += 4) V32(SDRAM_UBASE + b + o) = (o & 4) ? 0x0000FFFFu : 0xFFFF0000u;
    for(uint32_t o = 0; o < 64; o += 4) bad += V32(SDRAM_UBASE + b + o) != ((o & 4) ? 0x0000FFFFu : 0xFFFF0000u);
    return bad == 0;
}

/* Move dram_clk over one clock period in steps of 168 ps, initialise and check at each,
   then settle in the middle of the longest good run (around the circle on the Pocket) */
static void t_phase(void)
{
    int n = sim ? 4 : PHASE_STEPS_MAX, best = 0, first = 0;
    uint8_t good[PHASE_STEPS_MAX];
    uint32_t t0 = rdcycle(), before = begin(sd, "phase");
    for(int i = 0; i < n; i++) {
        if(i) phase_step(1);
        good[i] = sdram_reinit() && quick(0x1234567u + (uint32_t)i);
        phase_map[i] = good[i] ? '#' : '.';
    }
    phase_map[n] = 0;
    for(int s = 0; s < n; s++) {
        int len = 0;
        while(len < n && good[(s + len) % n] && (!sim || s + len < n)) len++;
        if(len > best) {
            best = len;
            first = s;
        }
    }
    int centre = best == n ? 0 : (first + best / 2) % n;   /* all good: stay at the start */
    int d = centre - (n - 1);
    if(!sim) {
        if(d > n / 2) d -= n;
        if(d < -n / 2) d += n;
    }
    for(; d > 0; d--) phase_step(1);
    for(; d < 0; d++) phase_step(0);
    if(!sdram_reinit()) fail(sd, 0, MEM_READY, MEM_STATUS);
    if(best < (sim ? 1 : 8)) fail(sd, 0, 8, (uint32_t)best);
    phase_first = first;
    phase_len = best;
    phase_centre = ((PHASE_STEPS(MEM_PHASE) % n) + n) % n;
    dlog_str("phase map ");
    dlog_str(phase_map);
    dlog_str("\nphase: good from step ");
    dlog_dec(first);
    dlog_str(", ");
    dlog_dec(best);
    dlog_str(" steps of 168 ps, now at step ");
    dlog_dec(phase_centre);
    dlog_char('\n');
    end(sd, B_SD_PHASE, before, t0);
}

/* Store an instruction and read it back as a line, the way the instruction cache will
   fetch it: code that a broken SDRAM changed is never run */
static int put_code(volatile uint32_t *p, uint32_t insn)
{
    uint32_t g;
    *p = insn;
    dcache_invalidate();
    if((g = *p) == insn) return 1;
    fail(sd, (uint32_t)(uintptr_t)p - SDRAM_BASE, insn, g);
    return 0;
}

/* Code in the SDRAM: run a copied function, then patch it with the old line still in the
   instruction cache each time */
static void t_code(void)
{
    uint32_t t0 = rdcycle(), before = begin(sd, "code"), off = 0x20000;
    volatile uint32_t *p = (volatile uint32_t *)(SDRAM_BASE + off);
    uint32_t (*f)(void) = (uint32_t (*)(void))(uintptr_t)p;
    if(put_code(p, 0x00000517u) && put_code(p + 1, 0x00008067u)) {    /* auipc a0, 0; ret */
        icache_sync();
        uint32_t r = f();
        if(r != (uint32_t)(uintptr_t)p) fail(sd, off, (uint32_t)(uintptr_t)p, r);
        for(uint32_t k = 1; k <= 4 && put_code(p, k << 20 | 0x00000513u); k++) {  /* li a0, k */
            icache_sync();
            if((r = f()) != k) fail(sd, off, k, r);
        }
    }
    end(sd, B_SD_CODE, before, t0);
}

/* Refresh: count the AUTO REFRESH commands over a second (two frames in the simulation) */
static void t_refresh(void)
{
    uint32_t frames = small ? 2 : 60, t0 = rdcycle(), before = begin(sd, "refresh");
    uint32_t f0 = VID_FRAME(VID_STATUS), r0;
    while(VID_FRAME(VID_STATUS) == f0)      /* start at a frame boundary */
        ;
    f0 = VID_FRAME(VID_STATUS);
    r0 = MEM_REFRESH;
    while(((VID_FRAME(VID_STATUS) - f0) & 0xFFFF) < frames)
        ;
    refresh_per_s = (MEM_REFRESH - r0) * 60 / frames;
    if(refresh_per_s < 128000) fail(sd, 0, 128000, refresh_per_s);
    dlog_str("SDRAM refresh per second ");
    dlog_dec((int32_t)refresh_per_s);
    dlog_char('\n');
    end(sd, B_SD_REFRESH, before, t0);
}

/* A read, a store, MEM_STATUS, all in one instruction cache line: a line fetch between the
   store and the status read would take longer than the store (16 clocks against 7) */
static uint32_t __attribute__((noipa, aligned(32)))
store_status(volatile uint32_t *read, volatile uint32_t *store, uint32_t v)
{
    (void)*read;
    *store = v;
    return MEM_STATUS;
}

/* The write status: a store that needs PRECHARGE and ACTIVATE first is not done yet when
   the next instruction reads MEM_STATUS */
static void t_status(void)
{
    uint32_t t0 = rdcycle(), before = begin(sd, "write status"), s, g;
    s = store_status(&V32(SDRAM_UBASE + 0x1000000),     /* bank 0, row 0x800 open */
                     &V32(SDRAM_BASE + 0x1800000), 0x600DF00Du);    /* bank 0, row 0xC00 */
    if(s & MEM_WRITES_IDLE) fail(sd, 0x1800000, 0, s);
    while(!(MEM_STATUS & MEM_WRITES_IDLE))
        ;
    if((g = V32(SDRAM_UBASE + 0x1800000)) != 0x600DF00Du) fail(sd, 0x1800000, 0x600DF00Du, g);
    end(sd, B_SD_STATUS, before, t0);
}

/* Timed code, aligned to instruction cache lines: a line is fetched at most once, before
   the first load (load_cycles) or in the first of many rounds (a data cache line each) */
static uint32_t __attribute__((noipa, aligned(32))) load_cycles(uint32_t a)
{
    uint32_t t = rdcycle();
    (void)V32(a);
    return rdcycle() - t;
}

static uint32_t __attribute__((noipa, aligned(32))) read_cycles(uint32_t a, uint32_t end)
{
    uint32_t t = rdcycle();
    for(; a < end; a += 32) {
        (void)V32(a);      (void)V32(a + 4);  (void)V32(a + 8);  (void)V32(a + 12);
        (void)V32(a + 16); (void)V32(a + 20); (void)V32(a + 24); (void)V32(a + 28);
    }
    return rdcycle() - t;
}

static uint32_t __attribute__((noipa, aligned(32))) write_cycles(uint32_t a, uint32_t end)
{
    uint32_t t = rdcycle();
    for(; a < end; a += 32) {
        V32(a) = a;      V32(a + 4) = a;  V32(a + 8) = a;  V32(a + 12) = a;
        V32(a + 16) = a; V32(a + 20) = a; V32(a + 24) = a; V32(a + 28) = a;
    }
    return rdcycle() - t;
}

/* Line reads (row hit and miss, the fastest of four tries without a refresh, which would
   close the row in between) and throughput through the cache */
static void measure(void)
{
    uint32_t n = sim ? 4096 : 65536, b = SDRAM_BASE + 0x300000;
    line_miss = line_hit = ~0u;
    for(uint32_t i = 0, tries = 0; i < 16 && tries < 4; i++) {
        uint32_t a = SDRAM_BASE + 0x2345000 + i * 0x40000, r = MEM_REFRESH, miss, hit; /* bank 2 */
        dcache_invalidate();
        (void)V32(a);                       /* opens a row */
        miss = load_cycles(a + 0x8000);     /* another row of the bank: PRECHARGE, ACTIVATE */
        hit = load_cycles(a + 0x8020);      /* the next line of that row */
        if(MEM_REFRESH != r) continue;
        tries++;
        if(miss < line_miss) line_miss = miss;
        if(hit < line_hit) line_hit = hit;
    }
    dcache_invalidate();
    read_mbs = (uint32_t)((uint64_t)n * 7425 / (read_cycles(b, b + n) * 100u));
    write_mbs = (uint32_t)((uint64_t)n * 7425 / (write_cycles(b, b + n) * 100u));
    dlog_str("SDRAM line read: row hit ");
    dlog_dec((int32_t)line_hit);
    dlog_str(" cycles, row miss ");
    dlog_dec((int32_t)line_miss);
    dlog_str("; read ");
    dlog_dec((int32_t)read_mbs);
    dlog_str(" MB/s, write ");
    dlog_dec((int32_t)write_mbs);
    dlog_str(" MB/s\n");
}

uint32_t memtest_run(uint32_t boot_crc, int full, void (*cb)(const char *what, uint32_t addr))
{
    progress = cb;
    sim = (SYS_FLAGS & FLAG_SIM) != 0;
    small = sim || !full;
    uint32_t st = MEM_STATUS, t0 = rdcycle();
    while(!(st & MEM_READY) && rdcycle() - t0 < CPU_HZ / 2) st = MEM_STATUS;
    dlog_str("memtest: SYS_FLAGS ");
    dlog_hex(SYS_FLAGS);
    dlog_str(", MEM_STATUS ");
    dlog_hex(st);
    dlog_str(", bridge words ");
    dlog_dec((int32_t)MEM_BRIDGE_WORDS);
    dlog_char('\n');

    if(st & MEM_READY) {
        results |= 1u << B_SD_READY;
        t_slot(boot_crc);               /* first: nothing has written the SDRAM in this run */
        t_phase();
        t_memory(sd);
        t_code();
        t_refresh();
        t_status();
        measure();
    } else {
        dlog_str("SDRAM not ready: its tests skipped\n");
    }
    t_memory(&mems[M_SRAM]);
    t_memory(&mems[M_PSRAM]);

    dlog_str("memtest: errors SDRAM ");
    dlog_dec((int32_t)errors[M_SDRAM]);
    dlog_str(", SRAM ");
    dlog_dec((int32_t)errors[M_SRAM]);
    dlog_str(", cram0 ");
    dlog_dec((int32_t)errors[M_PSRAM]);
    dlog_char('\n');
    return results << MT_BIT_FIRST;
}

/*
 * Screen rows
 */
static char *ok_word(char *p, const char *label, int bit)
{
    return fmt_str(fmt_str(p, label), (results >> bit) & 1 ? " ok " : " XX ");
}

int memtest_rows(char rows[][41], uint8_t fail[])
{
    static const char *const names[M_N] = { "SDRAM", "SRAM ", "cram0" };
    char *p;
    int n = 0;
    for(int i = 0; i < M_N; i++) {
        int b = mems[i].first_bit;
        p = fmt_str(rows[n], names[i]);
        if(i == M_SDRAM && !(results & 1u << B_SD_READY)) {
            fmt_str(p, " nicht bereit");
            fail[n++] = 1;
            continue;
        }
        p = ok_word(p, " Bus", b);
        p = ok_word(p, "Byte", b + 1);
        p = ok_word(p, "Adr", b + 2);
        ok_word(p, "Muster", b + 3);
        fail[n] = ((results >> b) & 15) != 15;
        n++;
        if(i == M_SDRAM) {
            p = fmt_str(rows[n], "      Slot ");
            p = fmt_str(p, slot_state == 1 ? "ok" : slot_state == 3 ? "-" : "FEHLER");
            p = fmt_str(p, " Phase ");
            p = fmt_udec(p, (uint32_t)phase_len);
            fmt_udec(fmt_str(p, "/80, Schritt "), (uint32_t)phase_centre);
            fail[n++] = !((results >> B_SD_SLOT) & (results >> B_SD_PHASE) & 1);
            p = ok_word(fmt_str(rows[n], "     "), " Code", B_SD_CODE);
            p = ok_word(p, "Refresh", B_SD_REFRESH);
            ok_word(p, "Status", B_SD_STATUS);
            fail[n] = ((results >> B_SD_CODE) & 7) != 7;
            n++;
            p = fmt_udec(fmt_str(rows[n], "      Zeile "), line_hit);
            p = fmt_udec(fmt_str(p, "/"), line_miss);
            p = fmt_udec(fmt_str(p, " Takte, "), read_mbs);
            fmt_str(fmt_udec(fmt_str(p, "/"), write_mbs), " MB/s");
            fail[n++] = 0;
        }
    }
    return n;
}
