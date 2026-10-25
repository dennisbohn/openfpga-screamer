/*
 * Step-1 demo (docs/soc.md): the CPU tests itself, measures and draws.
 * At boot: self-tests and cycle counts into the debug log. Then every frame into the back
 * buffer: palette, test band, background, text (results, cycle counts, frame counter, keys)
 * and a square that the D-pad moves; then a swap. After the third completed swap the
 * self-test result goes to SYS_SIM once (0x80000007 when everything passed).
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "selftest.h"

uint32_t frames_shown;             /* read by the trap handler */

#define BAND_H      8           /* test band: rows 0..7, index x for x < 256, else 0 */
#define SQ          16          /* the square, pixels */
#define SQ_STEP     2           /* pixels per frame while the D-pad is held */
#define LOOPS       1000        /* iterations of each benchmark loop */

extern const uint8_t __image_start[], __image_end[];   /* link.ld: what firmware.bin holds */

struct stats {
    int ok[3];                  /* self-tests: integer, float, CFU */
    uint32_t results;           /* the same as SYS_SIM bits 2..0 */
    uint32_t cyc_float, cyc_int;
    uint32_t cyc_draw, cyc_fill;    /* the last frame, and its background fill */
};

static int (*const tests[3])(void) = { selftest_int, selftest_float, selftest_cfu };
static const char *const test_names[3] = { "Ganzzahl", "Fliesskomma", "CFU" };
static const char *const log_names[3] = { "integer", "float", "cfu" };

/* CRC-32 as zlib computes it, half a byte at a time: names the build and checks the load */
static uint32_t crc32(const uint8_t *p, const uint8_t *end)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    uint32_t c = 0xFFFFFFFFu;
    while (p < end) {
        c ^= *p++;
        c = (c >> 4) ^ t[c & 15];
        c = (c >> 4) ^ t[c & 15];
    }
    return ~c;
}

/* LOOPS dependent multiply-adds (fmadd.s). The empty asm statements keep the loop between
   the two cycle reads. */
static uint32_t __attribute__((noinline)) bench_float(void)
{
    float acc = opaquef(1.0f), m = opaquef(0.999f), c = opaquef(0.5f);
    uint32_t t0 = rdcycle();
    __asm__ volatile ("" : "+f"(acc));
    for (int i = 0; i < LOOPS; i++)
        acc = acc * m + c;
    __asm__ volatile ("" : : "f"(acc));
    return rdcycle() - t0;
}

/* The same with integers: x = x * m + c (mul, add) */
static uint32_t __attribute__((noinline)) bench_int(void)
{
    uint32_t x = opaque(1), m = opaque(1664525), c = opaque(1013904223);
    uint32_t t0 = rdcycle();
    __asm__ volatile ("" : "+r"(x));
    for (int i = 0; i < LOOPS; i++)
        x = x * m + c;
    __asm__ volatile ("" : : "r"(x));
    return rdcycle() - t0;
}

static void log_head(uint32_t drawn)
{
    char buf[DLOG_HEAD + 16];
    fmt_udec(fmt_str(buf, "Screamer Pocket step 1, frames drawn: "), drawn);
    dlog_head(buf);
}

/* Spin until the last swap is done; returns VID_STATUS. Logs once if it takes 0.5 s. */
static uint32_t wait_swap_done(void)
{
    static int warned;
    uint32_t t0 = rdcycle(), st;
    while ((st = VID_STATUS) & VID_PENDING) {
        if (!warned && rdcycle() - t0 > CPU_HZ / 2) {
            warned = 1;
            dlog_str("swap still pending after 0.5 s, VID_STATUS=");
            dlog_hex(st);
            dlog_char('\n');
        }
    }
    return st;
}

static void draw_band(volatile uint8_t *fb)
{
    volatile uint32_t *p = (volatile uint32_t *)fb;
    for (int y = 0; y < BAND_H; y++)
        for (uint32_t x = 0; x < FB_W / 4; x++)     /* four pixels 4x .. 4x+3 per word */
            *p++ = x < 64 ? 0x03020100u + 0x04040404u * x : 0;
}

static void draw_value(volatile uint8_t *fb, int row, const char *label, uint32_t v)
{
    char buf[12];
    con_text(fb, 3, row, label, COL_TEXT, COL_BG);
    fmt_udec(buf, v);
    con_text(fb, 22, row, buf, COL_TEXT, COL_BG);
}

static void draw_text(volatile uint8_t *fb, const struct stats *s, uint32_t st, uint32_t keys,
                      uint32_t drawn)
{
    char buf[CON_COLS + 1];
    fb_fill(fb, 0, 2 * 8 - 3, FB_W, 8 + 6, COL_TEXT);  /* title bar, 3 pixels around the text */
    con_text(fb, 1, 2, "SCREAMER POCKET - CPU LAEUFT", COL_BG, COL_TEXT);

    con_text(fb, 1, 4, "Selbsttest", COL_DIM, COL_BG);
    for (int i = 0; i < 3; i++) {
        con_text(fb, 3, 5 + i, test_names[i], COL_TEXT, COL_BG);
        con_text(fb, 22, 5 + i, s->ok[i] ? "OK" : "FEHLER", COL_TEXT, s->ok[i] ? COL_BG : COL_ALERT);
    }

    con_text(fb, 1, 9, "Takte (rdcycle)", COL_DIM, COL_BG);
    draw_value(fb, 10, "1000 x fmadd.s", s->cyc_float);
    draw_value(fb, 11, "1000 x mul + add", s->cyc_int);
    draw_value(fb, 12, "Bild zeichnen", s->cyc_draw);
    draw_value(fb, 13, "davon Hintergrund", s->cyc_fill);

    fmt_udec(fmt_str(fmt_udec(fmt_str(buf, "Bild "), VID_FRAME(st)), "  gezeichnet "), drawn);
    con_text(fb, 1, 15, buf, COL_TEXT, COL_BG);
    fmt_hex(fmt_str(buf, "IN_KEY "), keys, 8);
    con_text(fb, 1, 16, buf, COL_TEXT, COL_BG);
    con_text(fb, 1, 24, "Steuerkreuz bewegt das Quadrat", COL_DIM, COL_BG);
}

int main(void)
{
    uint32_t crc = crc32(__image_start, __image_end);  /* first, before .data changes */
    static struct stats s;

    dlog_init();
    log_head(0);
    dlog_str("firmware.bin: ");     /* the same line as tools/fw.sh prints */
    dlog_dec((int32_t)(__image_end - __image_start));
    dlog_str(" bytes, crc32 ");
    dlog_hex(crc);
    uint32_t id = SYS_ID;
    dlog_str("\nSYS_ID=");
    dlog_hex(id);
    dlog_str(id == SYS_ID_VALUE ? " ok\n" : " wrong\n");

    for (int i = 0; i < 3; i++) {   /* one line each, so a trap shows which one ran */
        s.ok[i] = tests[i]();
        s.results |= (uint32_t)s.ok[i] << i;
        dlog_str("selftest ");
        dlog_str(log_names[i]);
        dlog_str(s.ok[i] ? ": ok\n" : ": FAIL\n");
    }

    bench_float();                  /* first runs fill the instruction cache */
    bench_int();
    s.cyc_float = bench_float();
    s.cyc_int = bench_int();
    dlog_str("cycles: 1000 x fmadd.s ");
    dlog_dec((int32_t)s.cyc_float);
    dlog_str(", 1000 x mul + add ");
    dlog_dec((int32_t)s.cyc_int);
    dlog_char('\n');

    int x = (FB_W - SQ) / 2, y = 160;
    uint32_t drawn = 0;
    for (;;) {
        uint32_t st = wait_swap_done();
        frames_shown = drawn;       /* every frame drawn so far has been swapped in */
        if (drawn == 3) {           /* three frames drawn and swapped */
            SYS_SIM = SIM_DONE | s.results;
            dlog_str("frame 3: draw ");
            dlog_dec((int32_t)s.cyc_draw);
            dlog_str(" cycles, background ");
            dlog_dec((int32_t)s.cyc_fill);
            dlog_str(" (");
            dlog_dec(FB_W * (FB_H - BAND_H) / 4);
            dlog_str(" word stores)\nSYS_SIM=");
            dlog_hex(SIM_DONE | s.results);
            dlog_char('\n');
        }

        uint32_t keys = IN_KEY;
        if ((keys & KEY_LEFT) && x > 0)
            x -= SQ_STEP;
        if ((keys & KEY_RIGHT) && x < FB_W - SQ)
            x += SQ_STEP;
        if ((keys & KEY_UP) && y > BAND_H)
            y -= SQ_STEP;
        if ((keys & KEY_DOWN) && y < FB_H - SQ)
            y += SQ_STEP;

        volatile uint8_t *fb = FB(!(st & VID_FRONT));  /* the back buffer */
        uint32_t t0 = rdcycle();
        pal_write();
        draw_band(fb);
        uint32_t t1 = rdcycle();
        fb_fill(fb, 0, BAND_H, FB_W, FB_H - BAND_H, COL_BG);
        s.cyc_fill = rdcycle() - t1;
        draw_text(fb, &s, st, keys, drawn);
        fb_fill(fb, x, y, SQ, SQ, COL_SQUARE);
        VID_SWAP = 1;
        s.cyc_draw = rdcycle() - t0;
        log_head(++drawn);
    }
}
