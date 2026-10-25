/*
 * Step-3 firmware (docs/soc.md, docs/speicher.md): the CPU tests itself and the memories,
 * then draws triangles with the rasterizer's C model (raster.c).
 * At boot: CPU self-tests, then the memory test of SDRAM, SRAM and PSRAM cram0 (quick, or
 * over the whole memories while Select is held) with its progress on screen and in the
 * debug log's head line. Then every frame into the back buffer: palette, test band,
 * background, two turning wheels of triangles that overlap and a triangle moved with the
 * d-pad, with their cycle count; while Select is held the memory test results instead.
 * Then a swap. After the third completed swap the results go to SYS_SIM once
 * (0x801FFFFF when everything passed).
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "selftest.h"
#include "memtest.h"
#include "raster.h"

uint32_t frames_shown;             /* read by the trap handler */

#define BAND_H      8           /* test band: rows 0..7, index x for x < 256, else 0 */
#define STACK_FILL  0x57AC57ACu /* marks unused stack, for the high-water mark */

extern const uint8_t __image_start[], __image_end[];   /* link.ld: what firmware.bin holds */
extern uint32_t __bss_end[];

static int ok[3];               /* self-tests: integer, float, CFU */
static uint32_t results;        /* SYS_SIM bits 20..0 */
static int (*const tests[3])(void) = { selftest_int, selftest_float, selftest_cfu };
static const char *const test_names[3] = { "Ganzzahl", "Fliesskomma", "CFU" };
static const char *const log_names[3] = { "integer", "float", "cfu" };
static volatile uint8_t *progress_fb;

/* CRC-32 as zlib computes it, half a byte at a time: names the build and checks the load */
uint32_t crc32(const uint8_t *p, const uint8_t *end)
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

/* Mark the free stack up to 256 bytes below the stack pointer, find how deep it was used
   later */
static void __attribute__((noinline)) stack_fill(void)
{
    uintptr_t sp;
    __asm__ volatile ("mv %0, sp" : "=r"(sp));
    for (uint32_t *p = __bss_end; (uintptr_t)p < sp - 256; p++)
        *p = STACK_FILL;
}

static uint32_t stack_used(void)
{
    uint32_t *p = __bss_end;
    while (*p == STACK_FILL)
        p++;
    return BOOTRAM_SIZE - (uint32_t)(uintptr_t)p;
}

static void log_head(const char *what, uint32_t n)
{
    char buf[DLOG_HEAD + 16];
    fmt_udec(fmt_str(fmt_str(buf, "Screamer Pocket step 3, "), what), n);
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

/* Everything but the parts that change: band, background, title, self-test results */
static void draw_frame(volatile uint8_t *fb, const char *title)
{
    pal_write();
    draw_band(fb);
    fb_fill(fb, 0, BAND_H, FB_W, FB_H - BAND_H, COL_BG);
    fb_fill(fb, 0, 2 * 8 - 3, FB_W, 8 + 6, COL_TEXT);  /* title bar, 3 pixels around the text */
    con_text(fb, 1, 2, title, COL_BG, COL_TEXT);
    int col = con_text(fb, 1, 4, "CPU", COL_DIM, COL_BG) + 1;
    for (int i = 0; i < 3; i++) {
        col = con_text(fb, col, 4, test_names[i], COL_TEXT, COL_BG) + 1;
        col = con_text(fb, col, 4, ok[i] ? "ok" : "XX", COL_TEXT, ok[i] ? COL_BG : COL_ALERT) + 1;
    }
}

/* memtest_run() calls this before each test and every 64 KB: on screen and in the log */
static void progress(const char *what, uint32_t addr)
{
    char buf[DLOG_HEAD + 16];
    fmt_hex(fmt_str(fmt_str(fmt_str(buf, "teste "), what), " bei "), addr, 8);
    fb_fill(progress_fb, 0, 8 * 8, FB_W, 8, COL_BG);
    con_text(progress_fb, 1, 8, buf, COL_TEXT, COL_BG);
    fmt_hex(fmt_str(fmt_str(fmt_str(buf, "Screamer Pocket step 3, "), what), " @"), addr, 8);
    dlog_head(buf);
}

static void draw_results(volatile uint8_t *fb)
{
    char rows[MT_ROWS][41];
    uint8_t fail[MT_ROWS];
    int n = memtest_rows(rows, fail);
    con_text(fb, 1, 6, "Speicher", COL_DIM, COL_BG);
    for (int i = 0; i < n; i++)
        con_text(fb, 1, 7 + i, rows[i], COL_TEXT, fail[i] ? COL_ALERT : COL_BG);
}

/*
 * Triangle demo: two wheels that turn opposite ways and overlap (the right one is drawn
 * later and covers the left), and a triangle on top that the d-pad moves.
 */
static int16_t sin_q14[256];    /* sin(2 pi i / 256), 1.0 = 16384 */

static void sin_init(void)
{
    const float dc = 0.99969881869620f, ds = 0.02454122852291f;    /* cos, sin of 2 pi / 256 */
    float c = 1.0f, s = 0.0f;
    for (int i = 0; i < 256; i++) {
        sin_q14[i] = (int16_t)(s * 16384.0f + (s < 0.0f ? -0.5f : 0.5f));
        float t = c * dc - s * ds;
        s = s * dc + c * ds;
        c = t;
    }
}

/* a point at radius r (pixels) and angle a (1/256 turn) around the centre (cx, cy), in 1/16 pixel */
static struct rs_vertex polar(int cx, int cy, int r, int a)
{
    struct rs_vertex v;
    v.x = cx * 16 + ((r * sin_q14[(a + 64) & 255]) >> 10);
    v.y = cy * 16 + ((r * sin_q14[a & 255]) >> 10);
    return v;
}

static const struct rs_clip demo_clip = { 0, 48, FB_W, 168 };  /* text rows 6..20 */
static int player_x = 160 * 16, player_y = 120 * 16;

struct demo_stats { uint32_t triangles, pixels, cycles; };

static struct demo_stats draw_demo(volatile uint8_t *fb, uint32_t frame, uint32_t keys)
{
    static const uint8_t left_cols[8] = { 0, 32, 64, 96, 128, 160, 224, 255 };
    static const uint8_t right_cols[6] = { 16, 48, 112, 144, 208, 240 };
    struct demo_stats d = { 0, 0, 0 };
    struct rs_vertex v[3];
    uint32_t t0 = rdcycle();
    int a = (int)frame;

    for (int k = 0; k < 8; k++) {           /* left wheel, clockwise */
        v[0] = polar(110, 108, 0, 0);
        v[1] = polar(110, 108, 58, a + 32 * k);
        v[2] = polar(110, 108, 34, a + 32 * k + 18);
        d.pixels += rs_triangle(fb, &demo_clip, v, left_cols[k]);
    }
    for (int k = 0; k < 6; k++) {           /* right wheel, anticlockwise, drawn over the left */
        v[0] = polar(200, 108, 0, 0);
        v[1] = polar(200, 108, 56, -a + 43 * k);
        v[2] = polar(200, 108, 56, -a + 43 * k + 24);
        d.pixels += rs_triangle(fb, &demo_clip, v, right_cols[k]);
    }

    /* the player's triangle: 2 pixels a frame, kept inside the demo area */
    if (keys & KEY_LEFT) player_x -= 32;
    if (keys & KEY_RIGHT) player_x += 32;
    if (keys & KEY_UP) player_y -= 32;
    if (keys & KEY_DOWN) player_y += 32;
    if (player_x < 12 * 16) player_x = 12 * 16;
    if (player_x > (FB_W - 12) * 16) player_x = (FB_W - 12) * 16;
    if (player_y < 60 * 16) player_y = 60 * 16;
    if (player_y > 156 * 16) player_y = 156 * 16;
    v[0] = (struct rs_vertex){ player_x, player_y - 12 * 16 };
    v[1] = (struct rs_vertex){ player_x + 11 * 16, player_y + 10 * 16 };
    v[2] = (struct rs_vertex){ player_x - 11 * 16, player_y + 10 * 16 };
    d.pixels += rs_triangle(fb, &demo_clip, v, COL_SQUARE);

    d.triangles = 8 + 6 + 1;
    d.cycles = rdcycle() - t0;
    return d;
}

static void draw_status(volatile uint8_t *fb, uint32_t st, uint32_t drawn, const struct demo_stats *d)
{
    char buf[CON_COLS + 1];
    int mem_ok = ((results >> MT_BIT_FIRST) & ((1u << MT_BITS) - 1)) == (1u << MT_BITS) - 1;
    con_text(fb, 1, 5, mem_ok ? "Speicher ok" : "Speicher FEHLER", COL_TEXT, mem_ok ? COL_BG : COL_ALERT);
    con_text(fb, 17, 5, "Select: Details", COL_DIM, COL_BG);
    if (d) {
        char *p = fmt_udec(fmt_str(buf, "Dreiecke "), d->triangles);
        p = fmt_udec(fmt_str(p, " Pixel "), d->pixels);
        fmt_udec(fmt_str(p, " Takte "), d->cycles);
        con_text(fb, 1, 22, buf, COL_TEXT, COL_BG);
    }
    fmt_udec(fmt_str(fmt_udec(fmt_str(buf, "Bild "), VID_FRAME(st)), "  gezeichnet "), drawn);
    con_text(fb, 1, 23, buf, COL_TEXT, COL_BG);
}

int main(void)
{
    uint32_t crc = crc32(__image_start, __image_end);  /* first, before .data changes */

    stack_fill();
    dlog_init();
    log_head("frames drawn: ", 0);
    dlog_str("firmware.bin: ");     /* the same line as tools/fw.sh prints */
    dlog_dec((int32_t)(__image_end - __image_start));
    dlog_str(" bytes, crc32 ");
    dlog_hex(crc);
    uint32_t id = SYS_ID;
    dlog_str("\nSYS_ID=");
    dlog_hex(id);
    dlog_str(id == SYS_ID_VALUE ? " ok\n" : " wrong\n");

    for (int i = 0; i < 3; i++) {   /* one line each, so a trap shows which one ran */
        ok[i] = tests[i]();
        results |= (uint32_t)ok[i] << i;
        dlog_str("selftest ");
        dlog_str(log_names[i]);
        dlog_str(ok[i] ? ": ok\n" : ": FAIL\n");
    }

#ifdef FAULT_IFETCH
    /* fault test for sim/soc (make FAULT=IFETCH): code past the end of the boot RAM. The
       trap handler has to run from lines the cache loads after the bad one (docs/soc.md). */
    dlog_str("fault test: jump to 00008000\n");
    ((void (*)(void))0x00008000u)();
#endif

    /* the memory test, its progress drawn into the buffer on screen */
    uint32_t st = wait_swap_done();
    progress_fb = FB(!(st & VID_FRONT));
    draw_frame(progress_fb, "SCREAMER POCKET - SPEICHERTEST");
    VID_SWAP = 1;
    wait_swap_done();
    results |= memtest_run(crc, (IN_KEY & KEY_SELECT) != 0, progress);

    sin_init();
    uint32_t drawn = 0;
    for (;;) {
        st = wait_swap_done();
        frames_shown = drawn;       /* every frame drawn so far has been swapped in */
        if (drawn == 3) {           /* three frames drawn and swapped */
            SYS_SIM = SIM_DONE | results;
            dlog_str("stack used ");
            dlog_dec((int32_t)stack_used());
            dlog_str(" bytes\nSYS_SIM=");
            dlog_hex(SIM_DONE | results);
            dlog_char('\n');
        }
        uint32_t keys = IN_KEY;
        volatile uint8_t *fb = FB(!(st & VID_FRONT));  /* the back buffer */
        draw_frame(fb, "SCREAMER POCKET - DREIECKE");
        if (keys & KEY_SELECT) {
            draw_results(fb);
            draw_status(fb, st, drawn, 0);
        } else {
            struct demo_stats d = draw_demo(fb, drawn, keys);
            draw_status(fb, st, drawn, &d);
            if (drawn == 2) {
                dlog_str("frame 2: ");
                dlog_dec((int32_t)d.triangles);
                dlog_str(" triangles, ");
                dlog_dec((int32_t)d.pixels);
                dlog_str(" pixels, ");
                dlog_dec((int32_t)d.cycles);
                dlog_str(" cycles\n");
            }
        }
        VID_SWAP = 1;
        log_head("frames drawn: ", ++drawn);
    }
}
