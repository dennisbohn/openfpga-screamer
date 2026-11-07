/*
 * Memory test (docs/memory.md; memtest.h lists the entry points). The generic tests (data bus,
 * byte lanes, address bus, patterns) take a memory that is described by struct memory: its
 * base addresses and its size. The core has one memory, the SDRAM, which also gets tests of its
 * own: the clock phase window, code, refresh and the write status. It is also where program.bin
 * is loaded. The APF is asked for that file only after the other tests (slot.h).
 *
 * The patterns go over the first and the last window of 2 KB, so that loop ends run too. In the
 * simulation (SYS_FLAGS) only the phase search and the measurements shrink. The patterns go
 * through the data cache, which is invalidated before each check. Every other test uses the
 * uncached addresses. Each error is logged with the value read and two uncached re-reads: the
 * same wrong value three times points to a write or address fault, and changing values point
 * to read timing.
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "memtest.h"
#include "crc32.h"
#include "slot.h"

/* Volatile accesses of a given width at an address */
#define V32(a)          (*(volatile uint32_t *)(uintptr_t)(a))
#define V16(a)          (*(volatile uint16_t *)(uintptr_t)(a))
#define V8(a)           (*(volatile uint8_t *)(uintptr_t)(a))
#define VS16(a)         (*(volatile int16_t *)(uintptr_t)(a))
#define VS8(a)          (*(volatile int8_t *)(uintptr_t)(a))

/* The result bits of the SDRAM tests. The bit numbers count from MT_BIT_FIRST in SYS_SIM
   (memtest.h). B_SD_DATA to B_SD_PATTERN are the four generic tests; the SDRAM's first_bit
   in struct memory is B_SD_DATA. */
enum {
    B_SD_READY, B_SD_SLOT, B_SD_PHASE, B_SD_DATA, B_SD_BYTES, B_SD_ADDR, B_SD_PATTERN,
    B_SD_CODE, B_SD_REFRESH, B_SD_STATUS,
};

/* The memories to test: only the SDRAM */
enum { M_SDRAM, M_N };

struct memory {
    const char *name;
    uint32_t base;              /* the cached view: the patterns are written and read here */
    uint32_t ubase;             /* the uncached view */
    uint32_t size;
    uint32_t die;               /* the windows repeat for each die of this size */
    uint8_t first_bit;          /* result bit of the data bus test; byte, address, pattern follow */
    uint8_t cached;             /* 1: invalidate the data cache before checking */
};

static const struct memory mems[M_N] = {
    { "SDRAM", SDRAM_BASE, SDRAM_UBASE, SDRAM_SIZE, SDRAM_SIZE, B_SD_DATA, 1 },
};

#define WINDOW          2048    /* bytes per window */
#define MAX_LOGGED      8       /* error lines per memory */
#define PHASE_STEPS_MAX 80      /* 80 steps of 168 ps = one clock period */

static uint32_t results;        /* the tests passed, one bit each, numbered as in the enum above */
static uint32_t errors[M_N];    /* errors counted per memory */
static int sim;                 /* 1 when running in the simulation */
static const char *test;        /* the name of the running test, for the log */
static void (*progress)(const char *what, uint32_t addr);   /* reports the test and the address (boot.c) */

/* The results that memtest_rows() shows on the screen. The longest good run of phase steps: its length;
   the step settled on. */
static int phase_len, phase_centre;
static uint32_t refresh_per_s, line_hit, line_miss, read_mbs, write_mbs;
static uint8_t slot_state;      /* the program.bin test: 0 not run, 1 ok, 2 failed */
static uint32_t prog_entry;     /* program.bin's entry address, set when its check passed */
static char phase_map[PHASE_STEPS_MAX + 1];             /* '#' for a good phase step, '.' for a bad one */

/* Count an error in memory m. The first MAX_LOGGED are also logged: the test, the address, the
   value wanted, the value got, and two uncached re-reads of the word. off is a byte offset in m. */
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

/* Start a test: remember its name, report the progress, and return the error count so far.
   end() takes that count back. */
static uint32_t begin(const struct memory *m, const char *name)
{
    test = name;
    progress(name, m->base);
    return errors[m - mems];
}

/* Finish a test. It passed if no error came since begin() returned `before`; then result
   bit `bit` is set. One log line gives the result and the cycles since t0. */
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

/* Data bus: a walking one and a walking zero on one word (uncached) */
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

/* Byte lanes (uncached). First: one partial store at a time into a word that was just filled
   with a background value; the word must then equal the background with only the stored bytes
   replaced. Then: sub-word reads of a known word at every offset, with sign and zero extension. */
static void t_bytes(const struct memory *m)
{
    /* Stores: the low two bits are the byte offset in the word, bit 4 marks a halfword store */
    static const uint8_t stores[6] = { 0, 1, 2, 3, 0x10, 0x12 };
    uint32_t t0 = rdcycle(), before = begin(m, "bytes"), off = 0x80, a = m->ubase + off;
    for(int inv = 0; inv < 2; inv++)            /* the second background is the complement of the first */
        for(int k = 0; k < 6; k++) {
            uint32_t bg = inv ? 0xA55A3CC3u : 0x5AA5C33Cu, o = stores[k] & 3, sh = 8 * o, want, g;
            V32(a) = bg;
            if(stores[k] & 0x10) {
                uint32_t v = 0x1234u ^ (uint32_t)k;     /* a different value for each store */
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
        uint32_t ub = V8(a + o);                /* lbu */
        int32_t sb = VS8(a + o);                /* lb */
        if(ub != want) fail(m, off + o, want, ub);
        if(sb != (int8_t)want) fail(m, off + o, (uint32_t)(int8_t)want, (uint32_t)sb);
        if(!(o & 1)) {                          /* halfwords only at even offsets */
            uint32_t wanth = (word >> 8 * o) & 0xFFFF;
            uint32_t uh = V16(a + o);           /* lhu */
            int32_t shw = VS16(a + o);          /* lh */
            if(uh != wanth) fail(m, off + o, wanth, uh);
            if(shw != (int16_t)wanth) fail(m, off + o, (uint32_t)(int16_t)wanth, (uint32_t)shw);
        }
    }
    end(m, m->first_bit + 1, before, t0);
}

/* The value that the address test stores at byte offset off. Each address gets its own value. */
static uint32_t addr_value(uint32_t off)
{
    return ~off ^ 0x13579BDFu;
}

/* Address bus (uncached): store a value of its own at offset 0 and at every power of two below
   the size, then read all of them back. This finds address and bank lines that stick or join. */
static void t_address(const struct memory *m)
{
    uint32_t t0 = rdcycle(), before = begin(m, "address");
    for(uint32_t o = 0; o < m->size; o = o ? o << 1 : 4)
        V32(m->ubase + o) = addr_value(o);
    for(uint32_t o = 0; o < m->size; o = o ? o << 1 : 4) {
        uint32_t g;
        if((g = V32(m->ubase + o)) != addr_value(o)) fail(m, o, addr_value(o), g);
    }
    end(m, m->first_bit + 2, before, t0);
}

/* The pattern tests go over the first and the last window of each die. They use five
   patterns, by `kind`; o is the byte offset of the word and x the random state:
     0  the offset xor a constant       3  0xFFFF0000
     1  the inverted offset xor it      4  0xAAAA5555
     2  pseudo-random (xorshift32)
   Kinds 3 and 4 switch all 16 data lines between the two halfwords of a word. */
static uint32_t xs;             /* the random state; fill() and check() continue from it */

#define XORSHIFT(x)     ((x) ^= (x) << 13, (x) ^= (x) >> 17, (x) ^= (x) << 5, (x))

static uint32_t next_random(void)
{
    return XORSHIFT(xs);
}

/* One loop per pattern, with no call inside, so that each word costs few instructions. (A
   pass over the whole SDRAM would be 16M words.) FILL and CHECK are the two loop bodies:
   FILL stores v(o) at offset o, CHECK reads it back and calls fail() on a difference. The
   error call is marked unlikely, so that the CPU's static branch prediction always hits
   (docs/memory.md). */
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

/* Write pattern `kind` over the byte offsets o .. end-1 of memory m (cached view) */
static void fill(const struct memory *m, int kind, uint32_t o, uint32_t end)
{
    uint32_t base = m->base, x = xs;
    PATTERN_LOOPS(FILL)
    xs = x;
}

/* Check pattern `kind` over the byte offsets o .. end-1. A pattern is checked with the same
   random state that wrote it. */
static void check(const struct memory *m, int kind, uint32_t o, uint32_t end)
{
    uint32_t base = m->base, x = xs;
    PATTERN_LOOPS(CHECK)
    xs = x;
}

/* Fill w with the (start, end) offsets of the windows: the first and the last WINDOW bytes of
   each die. Returns the number of windows. */
static int windows(const struct memory *m, uint32_t w[][2])
{
    int n = 0;
    for(uint32_t d = 0; d < m->size; d += m->die) {
        w[n][0] = d;
        w[n++][1] = d + WINDOW;
        w[n][0] = d + m->die - WINDOW;
        w[n++][1] = d + m->die;
    }
    return n;
}

/* Write (checking = 0) or check one pattern over all windows, 64 KB at a time between
   progress reports. Each window starts the random state again from its own start. */
static void pass(const struct memory *m, int kind, int checking, uint32_t w[][2], int nw)
{
    for(int i = 0; i < nw; i++) {
        uint32_t end = w[i][1];
        xs = 0x9E3779B9u ^ w[i][0] ^ (uint32_t)kind;
        for(uint32_t o = w[i][0]; o < end; o += 0x10000) {
            uint32_t e = end - o > 0x10000 ? o + 0x10000 : end;
            progress(test, m->base + o);
            if(checking) check(m, kind, o, e);
            else fill(m, kind, o, e);
        }
    }
}

/* Patterns. Each of the five kinds is written over the windows, the data cache is invalidated,
   and it is checked, so the check reads from the SDRAM. For a cached memory two more tests
   follow, described below. */
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
        /* First: the 64 lines of 32 bytes of the first window, in a scrambled order, so that
           SDRAM row misses follow right after writes. The first round writes the lines, the
           second reads them back, each round in a different order (the multipliers are odd, so
           each order visits every line once). */
        uint32_t lines = WINDOW / 32;
        for(int reading = 0; reading < 2; reading++) {
            for(uint32_t i = 0; i < lines; i++) {
                uint32_t line = (i * (reading ? 0x85EBCA77u : 0x9E3779B1u)) & (lines - 1);
                for(uint32_t o = line * 32; o < line * 32 + 32; o += 4) {
                    uint32_t v = o * 0x2545F491u, g;
                    if(!reading) V32(m->base + o) = v;
                    else if((g = V32(m->base + o)) != v) fail(m, o, v, g);
                }
            }
            dcache_invalidate();
        }
        /* Second: 256 random words anywhere in the memory, each stored and read back at once
           through the uncached view. The old word is put back, because data slots loaded into
           the SDRAM must stay intact. */
        xs = 0x2468ACE1u;
        for(int i = 0; i < 256; i++) {
            uint32_t o = next_random() & (m->size - 32) & ~31u, v = xs ^ 0x55AA55AAu, g;
            uint32_t old = V32(m->ubase + o);
            V32(m->base + o) = v;
            if((g = V32(m->ubase + o)) != v) fail(m, o, v, g);
            V32(m->base + o) = old;
        }
    }
    end(m, m->first_bit + 3, before, t0);
}

/* The four generic tests on memory m */
static void t_memory(const struct memory *m)
{
    dlog_str(m->name);
    dlog_str(sim ? ": simulation, windows of 2048 bytes at the start and the end\n"
                 : ": quick check, windows of 2048 bytes at the start and the end\n");
    t_databus(m);
    t_bytes(m);
    t_address(m);
    t_patterns(m);
}

/*
 * SDRAM only
 */
static const struct memory *const sd = &mems[M_SDRAM];

/* Re-initialise the SDRAM (PHASE_REINIT) and wait up to 0.1 s until it reports ready again.
   Returns 1 if it is ready, 0 on a timeout. */
static int sdram_reinit(void)
{
    MEM_PHASE = PHASE_REINIT;
    for(int i = 0; i < 1000 && (MEM_STATUS & MEM_READY); i++)   /* wait for the ready bit to drop */
        ;
    uint32_t t0 = rdcycle();
    while(!(MEM_STATUS & MEM_READY))
        if(rdcycle() - t0 > CPU_HZ / 10) return 0;
    return 1;
}

/* Move dram_clk by one step of 168 ps, later or earlier, and wait until the PLL has done it */
static void phase_step(int later)
{
    MEM_PHASE = PHASE_STEP | (later ? PHASE_LATER : 0);
    while(MEM_PHASE & PHASE_BUSY)
        ;
}

/* Load and check program.bin (data slot 2, deferload). The APF is asked for it only after the
   SDRAM clock phase has been found and the SDRAM tested. The 16-byte header is read first,
   for the size; then the whole file. The test checks the header, the size and the CRC-32. It
   fails too if the bridge lost a word while writing into the SDRAM. */
static void t_program(void)
{
    const volatile uint32_t *h = (const volatile uint32_t *)PROG_BASE;     /* the header words */
    uint32_t t0 = rdcycle(), before = begin(sd, "program"), size = 0;
    int e = slot_read(2, 0, PROG_BASE, 16);
    if(!e) {
        size = h[1];
        if(h[0] == PROG_MAGIC && size >= 16 && size <= PROG_MAX)
            e = slot_read(2, 0, PROG_BASE, (size + 3) & ~3u);   /* a multiple of 4, as slot_read needs */
    }
    if(e || h[0] != PROG_MAGIC || size < 16 || size > PROG_MAX) {
        dlog_str("program.bin: not loaded (data slot 2, result ");
        dlog_dec(e);
        dlog_str(")\n");
        fail(sd, PROG_BASE - SDRAM_BASE, PROG_MAGIC, h[0]);
    } else {
        uint32_t c = crc32((const uint8_t *)PROG_BASE + 16, (const uint8_t *)PROG_BASE + size);
        dlog_str("program.bin: ");
        dlog_dec((int32_t)size);
        dlog_str(" bytes, crc32 ");
        dlog_hex(c);
        dlog_char('\n');
        if(c != h[2]) fail(sd, PROG_BASE - SDRAM_BASE + 8, h[2], c);    /* h[2]: the CRC-32 in the header */
        else prog_entry = h[3];                                         /* h[3]: the entry address */
    }
    if(MEM_STATUS & (MEM_OVERFLOW | MEM_DROPPED)) {
        fail(sd, 0, 0, MEM_STATUS);
        prog_entry = 0;
    }
    end(sd, B_SD_SLOT, before, t0);
    slot_state = errors[M_SDRAM] == before ? 1 : 2;
}

uint32_t memtest_program(void)
{
    return prog_entry;
}

/* A short hard check for the phase sweep. First pseudo-random words go through the cache and
   are read back after an invalidate; then words that switch all data lines go through the
   uncached view. Returns 1 if every word read back right. */
static int quick(uint32_t seed)
{
    uint32_t n = sim ? 512 : 4096, b = 0x10000, bad = 0;
    dcache_invalidate();
    xs = seed;
    for(uint32_t o = 0; o < n; o += 4) V32(SDRAM_BASE + b + o) = next_random();
    dcache_invalidate();
    xs = seed;
    for(uint32_t o = 0; o < n; o += 4) bad += V32(SDRAM_BASE + b + o) != next_random();
    for(uint32_t o = 0; o < 64; o += 4)
        V32(SDRAM_UBASE + b + o) = (o & 4) ? 0x0000FFFFu : 0xFFFF0000u;
    for(uint32_t o = 0; o < 64; o += 4)
        bad += V32(SDRAM_UBASE + b + o) != ((o & 4) ? 0x0000FFFFu : 0xFFFF0000u);
    return bad == 0;
}

/* Find the SDRAM clock phase. Move dram_clk over one clock period in steps of 168 ps, and at
   each step re-initialise the SDRAM and check it with quick(). Then settle in the middle of
   the longest run of good steps (on the Pocket the run may wrap around the circle of steps).
   At a bad phase the chip may take commands and addresses wrongly and write anywhere, so
   nothing is loaded into the SDRAM before this test. */
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
    /* The longest run of good steps: for each start s, count the good steps in a row. Outside
       the simulation the run may wrap around the end of the sweep. */
    for(int s = 0; s < n; s++) {
        int len = 0;
        while(len < n && good[(s + len) % n] && (!sim || s + len < n)) len++;
        if(len > best) {
            best = len;
            first = s;
        }
    }
    int centre = best == n ? 0 : (first + best / 2) % n;   /* all good: use step 0, the start phase */
    /* The sweep ended at step n-1. d is the number of steps to move to the centre, later if
       positive. Outside the simulation take the shorter way around the circle. */
    int d = centre - (n - 1);
    if(!sim) {
        if(d > n / 2) d -= n;
        if(d < -n / 2) d += n;
    }
    for(; d > 0; d--) phase_step(1);
    for(; d < 0; d++) phase_step(0);
    if(!sdram_reinit()) fail(sd, 0, MEM_READY, MEM_STATUS);
    /* The longest good run must have at least 8 steps (1 in the simulation) */
    if(best < (sim ? 1 : 8)) fail(sd, 0, 8, (uint32_t)best);
    phase_len = best;
    phase_centre = ((PHASE_STEPS(MEM_PHASE) % n) + n) % n;      /* the step settled on, from the hardware */
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

/* Store the instruction insn at p and read it back as a line through the data cache, the way
   the instruction cache will fetch it. Returns 1 if it reads back right, else logs an error
   and returns 0. Code that a broken SDRAM changed is never run. */
static int put_code(volatile uint32_t *p, uint32_t insn)
{
    uint32_t g;
    *p = insn;
    dcache_invalidate();
    if((g = *p) == insn) return 1;
    fail(sd, (uint32_t)(uintptr_t)p - SDRAM_BASE, insn, g);
    return 0;
}

/* Code in the SDRAM. Write a function of two instructions there and run it. Then patch it four
   times with a new instruction, each time with the unpatched line still in the instruction
   cache until icache_sync(), and run it again. */
static void t_code(void)
{
    uint32_t t0 = rdcycle(), before = begin(sd, "code"), off = 0x20000;
    volatile uint32_t *p = (volatile uint32_t *)(SDRAM_BASE + off);
    uint32_t (*f)(void) = (uint32_t (*)(void))(uintptr_t)p;
    if(put_code(p, 0x00000517u) && put_code(p + 1, 0x00008067u)) {    /* auipc a0, 0; ret */
        icache_sync();
        uint32_t r = f();                       /* returns its own address */
        if(r != (uint32_t)(uintptr_t)p) fail(sd, off, (uint32_t)(uintptr_t)p, r);
        for(uint32_t k = 1; k <= 4 && put_code(p, k << 20 | 0x00000513u); k++) {  /* li a0, k */
            icache_sync();
            if((r = f()) != k) fail(sd, off, k, r);
        }
    }
    end(sd, B_SD_CODE, before, t0);
}

/* Refresh: count the AUTO REFRESH commands over two frames, starting at a frame boundary. At
   least 128000 a second must come (74.25 MHz / 512 = 145020 are expected). */
static void t_refresh(void)
{
    uint32_t frames = 2, t0 = rdcycle(), before = begin(sd, "refresh");
    uint32_t f0 = VID_FRAME(VID_STATUS), r0;
    while(VID_FRAME(VID_STATUS) == f0)      /* start at a frame boundary */
        ;
    f0 = VID_FRAME(VID_STATUS);
    r0 = MEM_REFRESH;
    while(((VID_FRAME(VID_STATUS) - f0) & 0xFFFF) < frames)     /* the frame counter has 16 bits */
        ;
    refresh_per_s = (MEM_REFRESH - r0) * VID_HZ / frames;
    if(refresh_per_s < 128000) fail(sd, 0, 128000, refresh_per_s);
    dlog_str("SDRAM refresh per second ");
    dlog_dec((int32_t)refresh_per_s);
    dlog_char('\n');
    end(sd, B_SD_REFRESH, before, t0);
}

/* A read, a store and a read of MEM_STATUS, all in one instruction cache line (the function
   is aligned to 32 bytes and not inlined). A line fetch between the store and the status read
   would take longer than the store (16 clocks against 7), and the status would then already
   say idle. */
static uint32_t __attribute__((noipa, aligned(32)))
store_status(volatile uint32_t *read, volatile uint32_t *store, uint32_t v)
{
    (void)*read;
    *store = v;
    return MEM_STATUS;
}

/* The write status. A store that needs PRECHARGE and ACTIVATE first is not done yet when the
   next instruction reads MEM_STATUS, so MEM_WRITES_IDLE must still be 0. After it comes up,
   the stored value must read back. */
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

/* The next three functions time memory accesses. They are aligned to instruction cache lines
   so that instruction fetches do not disturb the timing. A line of code is fetched at most
   once: before the timed load in load_cycles, and in the first round of the loop in the other
   two. That is one round of many; each round handles one data cache line. */
static uint32_t __attribute__((noipa, aligned(32))) load_cycles(uint32_t a)
{
    uint32_t t = rdcycle();
    (void)V32(a);
    return rdcycle() - t;
}

/* Cycles to read the bytes a .. end-1 through the cache, one 32-byte line per loop round */
static uint32_t __attribute__((noipa, aligned(32))) read_cycles(uint32_t a, uint32_t end)
{
    uint32_t t = rdcycle();
    for(; a < end; a += 32) {
        (void)V32(a);      (void)V32(a + 4);  (void)V32(a + 8);  (void)V32(a + 12);
        (void)V32(a + 16); (void)V32(a + 20); (void)V32(a + 24); (void)V32(a + 28);
    }
    return rdcycle() - t;
}

/* Cycles to write the bytes a .. end-1 through the cache, one 32-byte line per loop round */
static uint32_t __attribute__((noipa, aligned(32))) write_cycles(uint32_t a, uint32_t end)
{
    uint32_t t = rdcycle();
    for(; a < end; a += 32) {
        V32(a) = a;      V32(a + 4) = a;  V32(a + 8) = a;  V32(a + 12) = a;
        V32(a + 16) = a; V32(a + 20) = a; V32(a + 24) = a; V32(a + 28) = a;
    }
    return rdcycle() - t;
}

/* Measure the SDRAM (not a test) and log it. First the time of a cache line load with an open
   row (a row hit) and with a different row of the same bank (a row miss): the fastest of four
   tries. A try does not count if a refresh came in between, because a refresh closes the row.
   Then the throughput of reads and writes through the cache. */
static void measure(void)
{
    uint32_t n = sim ? 4096 : 65536, b = SDRAM_BASE + 0x300000;     /* n bytes for the throughput */
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
    /* MB/s = bytes / cycles * 74.25 MHz = n * 7425 / (cycles * 100) */
    read_mbs = n * 7425u / (read_cycles(b, b + n) * 100u);     /* n * 7425 < 2^32 */
    write_mbs = n * 7425u / (write_cycles(b, b + n) * 100u);
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

/* Run all tests (memtest.h) and return the result bits, shifted to MT_BIT_FIRST. The SDRAM
   gets up to 0.5 s to become ready; if it does not, its tests are skipped. */
uint32_t memtest_run(void (*cb)(const char *what, uint32_t addr))
{
    progress = cb;
    sim = (SYS_FLAGS & FLAG_SIM) != 0;
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
        t_phase();
        t_memory(sd);
        t_code();
        t_refresh();
        t_status();
        measure();
        t_program();                    /* last: program.bin is loaded after all other tests */
    } else {
        dlog_str("SDRAM not ready: its tests skipped\n");
    }

    dlog_str("memtest: errors SDRAM ");
    dlog_dec((int32_t)errors[M_SDRAM]);
    dlog_char('\n');
    return results << MT_BIT_FIRST;
}

/*
 * Screen rows
 */

/* Append the label and " ok " or " XX " (by result bit `bit`) to the string at p; returns the end */
static char *ok_word(char *p, const char *label, int bit)
{
    return fmt_str(fmt_str(p, label), (results >> bit) & 1 ? " ok " : " XX ");
}

/* Make the SDRAM's result rows for the screen (memtest.h): text in rows[], 40 characters at
   most; fail[n] is 1 for a row to highlight. Returns the number of rows. With a ready SDRAM
   there are four rows:
     SDRAM  Bus / Byte / Adr / Pattern  the four generic tests
     Prog and Phase                     program.bin, and the good phase steps of 80 and the step
     Code / Refresh / Status
     line hit/miss clocks, read/write MB/s   the measurements */
int memtest_rows(char rows[][41], uint8_t fail[])
{
    static const char *const names[M_N] = { "SDRAM" };
    char *p;
    int n = 0;
    for(int i = 0; i < M_N; i++) {
        int b = mems[i].first_bit;
        p = fmt_str(rows[n], names[i]);
        if(i == M_SDRAM && !(results & 1u << B_SD_READY)) {
            fmt_str(p, " not ready");
            fail[n++] = 1;
            continue;
        }
        p = ok_word(p, " Bus", b);
        p = ok_word(p, "Byte", b + 1);
        p = ok_word(p, "Adr", b + 2);
        ok_word(p, "Pattern", b + 3);
        fail[n] = ((results >> b) & 15) != 15;      /* the four generic tests */
        n++;
        if(i == M_SDRAM) {
            p = fmt_str(rows[n], "      Prog ");
            p = fmt_str(p, slot_state == 1 ? "ok" : "FAIL");
            p = fmt_str(p, " Phase ");
            p = fmt_udec(p, (uint32_t)phase_len);
            p = fmt_udec(fmt_str(p, "/"), PHASE_STEPS_MAX);
            fmt_udec(fmt_str(p, ", step "), (uint32_t)phase_centre);
            fail[n++] = !((results >> B_SD_SLOT) & (results >> B_SD_PHASE) & 1);    /* both must pass */
            p = ok_word(fmt_str(rows[n], "     "), " Code", B_SD_CODE);
            p = ok_word(p, "Refresh", B_SD_REFRESH);
            ok_word(p, "Status", B_SD_STATUS);
            fail[n] = ((results >> B_SD_CODE) & 7) != 7;    /* code, refresh, status: 3 adjacent bits */
            n++;
            p = fmt_udec(fmt_str(rows[n], "      line "), line_hit);
            p = fmt_udec(fmt_str(p, "/"), line_miss);
            p = fmt_udec(fmt_str(p, " clocks, "), read_mbs);
            fmt_str(fmt_udec(fmt_str(p, "/"), write_mbs), " MB/s");
            fail[n++] = 0;
        }
    }
    return n;
}
