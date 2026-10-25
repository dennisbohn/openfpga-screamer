/*
 * Step-3 firmware (docs/soc.md, docs/speicher.md): the CPU tests itself and the memories,
 * then draws triangles with the hardware rasterizer (raster.v), checked once a run
 * against its C model (raster.c).
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
#include "texdemo.h"

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
static int textures;            /* textures loaded into the SDRAM (data slot 3) */
static uint8_t col_bg = COL_BG, col_text = COL_TEXT, col_dim = COL_DIM, col_alert = COL_ALERT;

#define TEX_FILE    ((const volatile uint8_t *)(SDRAM_BASE + 0x2100000))  /* core/data.json */

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
    if (textures) {
        const volatile uint32_t *pal = texdemo_palette();
        for (int i = 0; i < 256; i++) VID_PAL[i] = pal[i];
    } else {
        pal_write();
    }
    draw_band(fb);
    fb_fill(fb, 0, BAND_H, FB_W, FB_H - BAND_H, col_bg);
    fb_fill(fb, 0, 2 * 8 - 3, FB_W, 8 + 6, col_text);  /* title bar, 3 pixels around the text */
    con_text(fb, 1, 2, title, col_bg, col_text);
    int col = con_text(fb, 1, 4, "CPU", col_dim, col_bg) + 1;
    for (int i = 0; i < 3; i++) {
        col = con_text(fb, col, 4, test_names[i], col_text, col_bg) + 1;
        col = con_text(fb, col, 4, ok[i] ? "ok" : "XX", col_text, ok[i] ? col_bg : col_alert) + 1;
    }
}

/* memtest_run() calls this before each test and every 64 KB: on screen and in the log */
static void progress(const char *what, uint32_t addr)
{
    char buf[DLOG_HEAD + 16];
    fmt_hex(fmt_str(fmt_str(fmt_str(buf, "teste "), what), " bei "), addr, 8);
    fb_fill(progress_fb, 0, 8 * 8, FB_W, 8, col_bg);
    con_text(progress_fb, 1, 8, buf, col_text, col_bg);
    fmt_hex(fmt_str(fmt_str(fmt_str(buf, "Screamer Pocket step 3, "), what), " @"), addr, 8);
    dlog_head(buf);
}

static void draw_results(volatile uint8_t *fb)
{
    char rows[MT_ROWS][41];
    uint8_t fail[MT_ROWS];
    int n = memtest_rows(rows, fail);
    con_text(fb, 1, 6, "Speicher", col_dim, col_bg);
    for (int i = 0; i < n; i++)
        con_text(fb, 1, 7 + i, rows[i], col_text, fail[i] ? col_alert : col_bg);
}

/*
 * The demo: the texture scene (texdemo.c) through the rasterizer, and on top a triangle in
 * one colour that the d-pad moves
 */
static const struct rs_clip demo_clip = { 0, 48, FB_W, 168 };  /* text rows 6..20 */
static int player_x = 160 * 16, player_y = 120 * 16;
static uint8_t col_player = COL_SQUARE;
static uint32_t draw_buf;               /* the back buffer, for hw_textured() */

struct demo_stats { uint32_t triangles, pixels, cycles; };

#define CHECK_BUF   ((volatile uint8_t *)(SDRAM_BASE + 0x100000))  /* the C model's picture */

/* the player's triangle: 2 pixels a frame, kept inside the demo area */
static void player(struct rs_vertex v[3], uint32_t keys)
{
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
}

/* one textured triangle to the rasterizer: the CPU only writes registers */
static uint32_t hw_textured(const struct rs_vertex pos[3], const struct rs_tex_setup *s, const struct rs_texture *t)
{
    for (int k = 0; k < 3; k++) RS_V(k) = RS_XY(pos[k].x, pos[k].y);
    RS_TEX = (uint32_t)((uintptr_t)t->data - SDRAM_BASE) >> 8;
    RS_TEXSIZE = (uint32_t)t->width | (uint32_t)t->height << 16 | (t->routine == 64 ? RS_CUT : 0);
    RS_REF = RS_XY(s->xr, s->yr);
    for (int k = 0; k < 3; k++) {
        RS_A(k) = (uint32_t)s->a[k];
        RS_DX(k) = (uint32_t)s->dx[k];
        RS_DY(k) = (uint32_t)s->dy[k];
    }
    RS_DRAW = draw_buf << 8 | RS_TEXTURED;      /* waits while the previous one draws */
    return 0;
}

static struct demo_stats draw_demo(uint32_t frame, const struct rs_vertex pv[3], int buf)
{
    struct demo_stats d;
    uint32_t t0 = rdcycle();
    draw_buf = (uint32_t)buf;
    RS_PIXELS = 0;
    RS_CLIP0 = RS_XY(demo_clip.x0, demo_clip.y0);
    RS_CLIP1 = RS_XY(demo_clip.x1, demo_clip.y1);
    texdemo_mode = TEXDEMO_HW;
    texdemo_draw(0, &demo_clip, frame, &d.triangles);
    for (int k = 0; k < 3; k++) RS_V(k) = RS_XY(pv[k].x, pv[k].y);
    RS_DRAW = col_player | (uint32_t)buf << 8;
    while (RS_DRAW & RS_BUSY)
        ;
    d.cycles = rdcycle() - t0;
    d.pixels = RS_PIXELS;
    d.triangles += 1;
    return d;
}

/* Once per run: the same picture through the C model into the SDRAM, then the demo area of
   both compared pixel by pixel */
static void raster_check(volatile uint8_t *fb, uint32_t frame, const struct rs_vertex pv[3], uint32_t hw_pixels)
{
    volatile uint8_t *m = CHECK_BUF;
    uint32_t pixels, differ = 0, first = 0, tris;
    fb_fill(m, demo_clip.x0, demo_clip.y0, demo_clip.x1 - demo_clip.x0, demo_clip.y1 - demo_clip.y0, col_bg);
    texdemo_mode = TEXDEMO_FIXED;
    pixels = texdemo_draw(m, &demo_clip, frame, &tris);
    pixels += rs_triangle(m, &demo_clip, pv, col_player);
    for (int y = demo_clip.y0; y < demo_clip.y1; y++)
        for (int x = demo_clip.x0; x < demo_clip.x1; x++)
            if (fb[y * FB_W + x] != m[y * FB_W + x] && differ++ == 0) first = (uint32_t)(y * FB_W + x);
    dlog_str("raster check: ");
    dlog_str(differ || pixels != hw_pixels ? "FAIL, " : "ok, ");
    dlog_dec((int32_t)differ);
    dlog_str(" pixels differ from the C model, pixels drawn ");
    dlog_dec((int32_t)hw_pixels);
    dlog_str(" (model ");
    dlog_dec((int32_t)pixels);
    dlog_str(")");
    if (differ) {
        dlog_str(", first at x ");
        dlog_dec((int32_t)(first % FB_W));
        dlog_str(" y ");
        dlog_dec((int32_t)(first / FB_W));
        dlog_str(": ");
        dlog_hex(fb[first]);
        dlog_str(" model ");
        dlog_hex(m[first]);
    }
    dlog_char('\n');
}

/* the palette entry nearest to an RGB colour, for text on the track's palette */
static uint8_t nearest(const volatile uint32_t *pal, int r, int g, int b)
{
    uint32_t best = ~0u;
    uint8_t n = 0;
    for (int i = 0; i < 256; i++) {
        int dr = (int)(pal[i] >> 16 & 255) - r, dg = (int)(pal[i] >> 8 & 255) - g, db = (int)(pal[i] & 255) - b;
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best) { best = d; n = (uint8_t)i; }
    }
    return n;
}

static void draw_status(volatile uint8_t *fb, uint32_t st, uint32_t drawn, const struct demo_stats *d)
{
    char buf[CON_COLS + 1];
    int mem_ok = ((results >> MT_BIT_FIRST) & ((1u << MT_BITS) - 1)) == (1u << MT_BITS) - 1;
    con_text(fb, 1, 5, mem_ok ? "Speicher ok" : "Speicher FEHLER", col_text, mem_ok ? col_bg : col_alert);
    con_text(fb, 17, 5, "Select: Details", col_dim, col_bg);
    if (d) {
        char *p = fmt_udec(fmt_str(buf, "Dreiecke "), d->triangles);
        p = fmt_udec(fmt_str(p, " Pixel "), d->pixels);
        fmt_udec(fmt_str(p, " Takte "), d->cycles);
        con_text(fb, 1, 22, buf, col_text, col_bg);
    }
    /* frames drawn in the last second (60 video frames) */
    static uint32_t fps, fps_drawn, fps_frame;
    if (((VID_FRAME(st) - fps_frame) & 0xFFFF) >= 60) {
        fps = drawn - fps_drawn;
        fps_drawn = drawn;
        fps_frame = VID_FRAME(st);
    }
    char *q = fmt_udec(fmt_str(fmt_udec(fmt_str(buf, "Bild "), VID_FRAME(st)), "  gezeichnet "), drawn);
    fmt_udec(fmt_str(q, "  FPS "), fps);
    con_text(fb, 1, 23, buf, col_text, col_bg);
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
    textures = texdemo_init(TEX_FILE);
    if (textures) {
        const volatile uint32_t *pal = texdemo_palette();
        col_bg = nearest(pal, 0, 0, 0);
        col_text = nearest(pal, 255, 255, 255);
        col_dim = nearest(pal, 120, 200, 120);
        col_alert = nearest(pal, 230, 25, 178);
        col_player = nearest(pal, 85, 170, 255);
        dlog_str("textures: ");
        dlog_dec(textures);
        dlog_str(" from data slot 3\n");
    } else {
        /* no textures.bin: test patterns instead, written before the rasterizer reads them */
        texdemo_synth((volatile uint8_t *)TEX_FILE);
        while (!(MEM_STATUS & MEM_WRITES_IDLE))
            ;
        textures = texdemo_init(TEX_FILE);
        dlog_str("textures: none in data slot 3, test patterns instead\n");
    }
    RS_CTRL = 1;                    /* the texture cache forgets what it held */
    texdemo_hw = hw_textured;

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
        draw_frame(fb, "SCREAMER POCKET - TEXTUREN");
        if (keys & KEY_SELECT) {
            draw_results(fb);
            draw_status(fb, st, drawn, 0);
        } else {
            struct rs_vertex pv[3];
            player(pv, keys);
            struct demo_stats d = draw_demo(drawn, pv, !(st & VID_FRONT));
            if (drawn == 2) raster_check(fb, drawn, pv, d.pixels);
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
