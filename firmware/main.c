/*
 * The program (program.bin, data slot 2) in the SDRAM, started by the boot loader with its
 * results (docs/soc.md). With a track in data slot 3 (tools/make_track.py) a flythrough of
 * it as a benchmark: the CPU does the geometry (track.c), the rasterizer draws; the d-pad
 * sets the speed. Without one, the texture scene with test patterns, checked once a run
 * against the rasterizer's C model (raster.c), and a triangle moved with the d-pad.
 * Every frame into the back buffer: palette, test band, background, the scene, the counts
 * and FPS; then a swap. After the third completed swap the results go to SYS_SIM once
 * (0x801FFFFF when everything passed).
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "memtest.h"
#include "raster.h"
#include "texdemo.h"
#include "screen.h"
#include "slot.h"
#include "track.h"

uint32_t frames_shown;             /* read by the trap handler */

#define STACK_FILL  0x57AC57ACu /* marks unused stack, for the high-water mark */
#define STACK_LOW   0x6000u     /* the stack may grow down to here (link.ld keeps the boot loader below) */

static uint32_t results;        /* SYS_SIM bits 20..0, from the boot loader */
static int textures;            /* textures in the SDRAM (data slot 3, or test patterns) */
static uint8_t col_bg = COL_BG, col_text = COL_TEXT, col_dim = COL_DIM, col_alert = COL_ALERT;

#define TEX_FILE    ((const volatile uint8_t *)(SDRAM_BASE + 0x2100000))  /* data slot 3 (core/data.json) */

/* Mark the free stack in the boot RAM up to 256 bytes below the stack pointer, find how
   deep it was used later */
static void __attribute__((noinline)) stack_fill(void)
{
    uintptr_t sp;
    __asm__ volatile ("mv %0, sp" : "=r"(sp));
    for (uint32_t *p = (uint32_t *)STACK_LOW; (uintptr_t)p < sp - 256; p++)
        *p = STACK_FILL;
}

static uint32_t stack_used(void)
{
    uint32_t *p = (uint32_t *)STACK_LOW;
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

/* Everything but the parts that change: palette, band, background, title, the boot
   loader's results */
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
    int cpu_ok = (results & 7) == 7;
    int mem_ok = ((results >> MT_BIT_FIRST) & ((1u << MT_BITS) - 1)) == (1u << MT_BITS) - 1;
    int col = con_text(fb, 1, 4, cpu_ok ? "CPU ok" : "CPU FEHLER", col_text, cpu_ok ? col_bg : col_alert) + 1;
    con_text(fb, col, 4, mem_ok ? "Speicher ok" : "Speicher FEHLER", col_text, mem_ok ? col_bg : col_alert);
}

/*
 * The demo: the texture scene (texdemo.c) through the rasterizer, and on top a triangle in
 * one colour that the d-pad moves
 */
static const struct rs_clip demo_clip = { 0, 48, FB_W, 168 };  /* text rows 6..20 */
static int player_x = 160 * 16, player_y = 120 * 16;
static uint8_t col_player = COL_SQUARE;
static uint32_t draw_buf;               /* the back buffer, for hw_textured() */
static uint32_t draw_wait;              /* cycles the DRAW writes waited for the rasterizer */

struct demo_stats { uint32_t triangles, pixels, cycles; };

#define CHECK_BUF   ((volatile uint8_t *)(SDRAM_BASE + 0x100000))  /* the C model's picture */
#define MESH_MEM    ((volatile uint8_t *)(SDRAM_BASE + 0x1F00000)) /* the test scene's cube */

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

/* the projection into the setup unit's registers */
static void su_view_regs(const struct su_view *v)
{
    SU_CENTRE = RS_XY(v->cx, v->cy);
    SU_SCALE = v->scale;
    SU_FAR = (uint32_t)v->far8;
}

/* a polygon in view space to the setup unit: its mode (with the number of corners) and
   texture, then the corners; the CPU only writes registers, a corner waits while the
   unit's input still holds the polygon before */
static void hw_poly(const struct su_corner *c, int n, const struct rs_texture *t, uint8_t colour)
{
    if (t) {
        SU_MODE = draw_buf << 8 | RS_TEXTURED | SU_CORNERS(n);
        SU_TEX = (uint32_t)((uintptr_t)t->data - SDRAM_BASE);
        SU_TEXSIZE = (uint32_t)t->width | (uint32_t)t->height << 16 | (t->routine == 64 ? RS_CUT : 0);
    } else {
        SU_MODE = colour | draw_buf << 8 | SU_CORNERS(n);
    }
    for (int i = 0; i < n; i++) {
        SU_R = c[i].r;
        SU_U = c[i].u;
        SU_F = c[i].f;
        SU_S = c[i].s;
        uint32_t t0 = rdcycle();
        if (i) SU_T_NEXT = c[i].t;
        else SU_T_FIRST = c[i].t;
        draw_wait += rdcycle() - t0;
    }
}

static uint32_t hw_quad(const struct su_corner c[4], const struct rs_texture *t)
{
    hw_poly(c, 4, t, 0);
    return 0;
}

/* a piece to the geometry unit: its place in view space and its mesh; the start waits
   while the unit is busy with the piece before */
static void hw_piece(const struct gu_piece *g)
{
    for (int k = 0; k < 9; k++) GU_M(k) = (uint32_t)g->m[k];
    for (int k = 0; k < 3; k++) {
        GU_T(k) = (uint32_t)g->t[k];
        GU_E(k) = (uint32_t)g->e[k];
    }
    GU_OBJ = (uint32_t)((uintptr_t)g->obj - SDRAM_BASE);
    GU_BLK = (uint32_t)((uintptr_t)g->blk - SDRAM_BASE);
    uint32_t t0 = rdcycle();
    GU_GO = (uint32_t)g->row;
    draw_wait += rdcycle() - t0;
}

static struct demo_stats draw_demo(uint32_t frame, const struct rs_vertex pv[3], int buf)
{
    struct demo_stats d;
    uint32_t t0 = rdcycle();
    draw_buf = (uint32_t)buf;
    RS_PIXELS = 0;
    RS_CLIP0 = RS_XY(demo_clip.x0, demo_clip.y0);
    RS_CLIP1 = RS_XY(demo_clip.x1, demo_clip.y1);
    struct su_view v;
    texdemo_view(&demo_clip, &v);
    su_view_regs(&v);
    GU_MLB = (uint32_t)((uintptr_t)TEX_FILE - SDRAM_BASE);
    GU_MODE = (uint32_t)buf << 8;
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
    if (d) {
        char *p = fmt_udec(fmt_str(buf, "Dreiecke "), d->triangles);
        p = fmt_udec(fmt_str(p, " Pixel "), d->pixels);
        fmt_udec(fmt_str(p, " Takte "), d->cycles);
        con_text(fb, 1, 22, buf, col_text, col_bg);
    }
    /* frames drawn in the last second */
    static uint32_t fps, fps_drawn, fps_frame;
    if (((VID_FRAME(st) - fps_frame) & 0xFFFF) >= VID_HZ) {
        fps = drawn - fps_drawn;
        fps_drawn = drawn;
        fps_frame = VID_FRAME(st);
    }
    char *q = fmt_udec(fmt_str(fmt_udec(fmt_str(buf, "Bild "), VID_FRAME(st)), "  gezeichnet "), drawn);
    fmt_udec(fmt_str(q, "  FPS "), fps);
    con_text(fb, 1, 23, buf, col_text, col_bg);
}

/*
 * The flythrough: the camera drives the player's line; every frame the counts on screen,
 * every 250 frames a line in the log
 */
static const struct rs_clip track_clip = { 0, 30, FB_W, 176 };

/* Once per run: the frame just drawn against the C models of the setup unit and the
   rasterizer, drawn into the SDRAM; the track area compared pixel by pixel */
static void track_check(volatile uint8_t *fb, uint32_t pos, uint8_t sky)
{
    volatile uint8_t *m = CHECK_BUF;
    struct track_stats ts;
    uint32_t differ = 0, first = 0, t0 = rdcycle();
    fb_fill(m, track_clip.x0, track_clip.y0, track_clip.x1 - track_clip.x0, track_clip.y1 - track_clip.y0, sky);
    track_mode = TRACK_MODEL;
    track_draw(m, &track_clip, pos, &ts);
    track_mode = TRACK_HW;
    for (int y = track_clip.y0; y < track_clip.y1; y++)
        for (int x = track_clip.x0; x < track_clip.x1; x++)
            if (fb[y * FB_W + x] != m[y * FB_W + x] && differ++ == 0) first = (uint32_t)(y * FB_W + x);
    dlog_str("track check: ");
    dlog_str(differ ? "FAIL, " : "ok, ");
    dlog_dec((int32_t)differ);
    dlog_str(" pixels differ from the C models (");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms)");
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

static void __attribute__((noreturn)) run_track(void)
{
    const uint32_t *pal = track_palette();
    uint8_t col_sky = nearest((const volatile uint32_t *)pal, 110, 150, 210);
    uint32_t lap = track_path_length() << 8, pos = 0, speed = 512, drawn = 0;
    uint32_t fps = 0, fps_drawn = 0, fps_frame = 0;
    struct su_view view;
    track_view(&track_clip, &view);
    track_mode = TRACK_HW;
    track_hw_piece = hw_piece;
    GU_MLB = (uint32_t)((uintptr_t)track_mlb() - SDRAM_BASE);
    GU_INVAL = 1;
    for (;;) {
        uint32_t st = wait_swap_done(), keys = IN_KEY;
        frames_shown = drawn;
        if (drawn == 3) {
            SYS_SIM = SIM_DONE | results;
            dlog_str("stack used ");
            dlog_dec((int32_t)stack_used());
            dlog_str(" bytes\nSYS_SIM=");
            dlog_hex(SIM_DONE | results);
            dlog_char('\n');
        }
        if ((keys & KEY_UP) && speed < 4096) speed += 16;
        if ((keys & KEY_DOWN) && speed >= 16) speed -= 16;

        volatile uint8_t *fb = FB(!(st & VID_FRONT));
        for (int i = 0; i < 256; i++) VID_PAL[i] = pal[i];
        draw_band(fb);
        fb_fill(fb, 0, BAND_H, FB_W, track_clip.y0 - BAND_H, col_bg);
        fb_fill(fb, 0, track_clip.y0, FB_W, track_clip.y1 - track_clip.y0, col_sky);
        fb_fill(fb, 0, track_clip.y1, FB_W, FB_H - track_clip.y1, col_bg);
        con_text(fb, 1, 2, "SCREAMER POCKET - STRECKE 1", col_text, col_bg);

        /* the pieces from the CPU to the geometry unit, its faces to the setup unit, the
           triangles to the rasterizer */
        struct track_stats ts;
        uint32_t t0 = rdcycle();
        draw_buf = !(st & VID_FRONT);
        RS_PIXELS = 0;
        RS_MISSES = 0;
        RS_CLOCKS = 0;
        SU_CLOCKS = 0;
        GU_CLOCKS = 0;
        GU_FACES = 0;
        GU_MODE = draw_buf << 8;
        draw_wait = 0;
        RS_CLIP0 = RS_XY(track_clip.x0, track_clip.y0);
        RS_CLIP1 = RS_XY(track_clip.x1, track_clip.y1);
        su_view_regs(&view);
        track_draw(0, &track_clip, pos, &ts);
        while (RS_DRAW & RS_BUSY)
            ;
        uint32_t cycles = rdcycle() - t0, raster = RS_CLOCKS, pixels = RS_PIXELS, misses = RS_MISSES;
        uint32_t setup = SU_CLOCKS, geom = GU_CLOCKS;
        ts.faces = GU_FACES;
        if (drawn == 2) track_check(fb, pos, col_sky);

        if (((VID_FRAME(st) - fps_frame) & 0xFFFF) >= VID_HZ) {
            fps = drawn - fps_drawn;
            fps_drawn = drawn;
            fps_frame = VID_FRAME(st);
        }
        char buf[CON_COLS + 1], *p;
        p = fmt_udec(fmt_str(buf, "Teile "), ts.pieces);
        fmt_udec(fmt_str(p, " Flaechen "), ts.faces);
        con_text(fb, 1, 22, buf, col_text, col_bg);
        p = fmt_udec(fmt_str(buf, "CPU "), cycles);
        p = fmt_udec(fmt_str(p, " Raster "), raster);
        fmt_udec(fmt_str(p, " Miss "), misses);
        con_text(fb, 1, 23, buf, col_text, col_bg);
        p = fmt_udec(fmt_str(buf, "Pixel "), pixels);
        p = fmt_udec(fmt_str(p, " FPS "), fps);
        fmt_udec(fmt_str(p, " Tempo "), speed >> 4);
        con_text(fb, 1, 24, buf, col_text, col_bg);
        if (drawn % 250 == 2) {
            dlog_str("track frame ");
            dlog_dec((int32_t)drawn);
            dlog_str(": point ");
            dlog_dec((int32_t)(pos >> 8));
            dlog_str(", ");
            dlog_dec((int32_t)ts.pieces);
            dlog_str(" pieces, ");
            dlog_dec((int32_t)ts.faces);
            dlog_str(" faces, ");
            dlog_dec((int32_t)pixels);
            dlog_str(" px, cpu ");
            dlog_dec((int32_t)cycles);
            dlog_str(", geometry ");
            dlog_dec((int32_t)geom);
            dlog_str(", setup ");
            dlog_dec((int32_t)setup);
            dlog_str(", raster ");
            dlog_dec((int32_t)raster);
            dlog_str(", misses ");
            dlog_dec((int32_t)misses);
            dlog_str(", fps ");
            dlog_dec((int32_t)fps);
            if (drawn == 2) {                   /* the log has 4 KB: once */
                dlog_str(", camera ");
                dlog_dec(ts.cam[0]);
                dlog_char(' ');
                dlog_dec(ts.cam[1]);
                dlog_char(' ');
                dlog_dec(ts.cam[2]);
                dlog_str(", nearest ");
                dlog_dec((int32_t)ts.nearest);
            }
            dlog_str("\n  cycles: cull ");
            dlog_dec((int32_t)ts.cyc_cull);
            dlog_str(", pieces ");
            dlog_dec((int32_t)ts.cyc_piece);
            dlog_str(", out ");
            dlog_dec((int32_t)ts.cyc_out);
            dlog_str(" (wait ");
            dlog_dec((int32_t)draw_wait);
            dlog_str("), after ");
            dlog_dec((int32_t)(cycles - ts.cyc_cull - ts.cyc_piece - ts.cyc_out));
            dlog_str("\n");
        }
        VID_SWAP = 1;
        log_head("frames drawn: ", ++drawn);
        pos = (pos + speed) % lap;
    }
}

void prog_main(uint32_t boot_results, uint32_t log_pos, uint32_t log_cur)
{
    results = boot_results;
    stack_fill();
    dlog_attach(log_pos, log_cur);
    log_head("frames drawn: ", 0);
    dlog_str("program.bin: running in the SDRAM\n");

    /* the track (data slot 3, deferload): its header has the file's size; the file is only
       read once all its blocks are as written */
    uint32_t t_load = rdcycle();
    int placements = 0, e = slot_read(3, 0, (uint32_t)(uintptr_t)TEX_FILE, 0x100);
    if (!e && track_open(TEX_FILE)) e = slot_read(3, 0, (uint32_t)(uintptr_t)TEX_FILE, (track_size() + 3) & ~3u);
    t_load = (rdcycle() - t_load) / (CPU_HZ / 1000);
    if (e) {
        dlog_str("track1.bin: not loaded (data slot 3, result ");
        dlog_dec(e);
        dlog_str(")\n");
    } else if (track_open(TEX_FILE)) {
        dlog_str("track1.bin: ");
        dlog_dec((int32_t)track_size());
        dlog_str(" bytes loaded in ");
        dlog_dec((int32_t)t_load);
        dlog_str(" ms, CRC of the 256 KB blocks:");
        int bad = 0;
        for (int k = 0; k < track_blocks(); k++) {
            int ok = track_block_ok(k);
            bad += !ok;
            dlog_str(ok ? " ok" : " BAD");
        }
        dlog_str(bad ? " - damaged in the SDRAM, not used\n" : " (all ok)\n");
        if (!bad) placements = track_init();
    }
    if (placements) {
        const volatile uint32_t *pal = (const volatile uint32_t *)track_palette();
        col_bg = nearest(pal, 0, 0, 0);
        col_text = nearest(pal, 255, 255, 255);
        col_dim = nearest(pal, 120, 200, 120);
        col_alert = nearest(pal, 230, 25, 178);
        dlog_str("track: ");
        dlog_dec(placements);
        dlog_str(" placements, ");
        dlog_dec((int32_t)track_path_length());
        dlog_str(" path points (data slot 3)\n");
        RS_CTRL = 1;
        run_track();
    }
    /* no track: the scene with test patterns, written before the rasterizer reads them */
    texdemo_synth((volatile uint8_t *)TEX_FILE);
    while (!(MEM_STATUS & MEM_WRITES_IDLE))
        ;
    textures = texdemo_init(TEX_FILE);
    texdemo_mesh((volatile uint8_t *)TEX_FILE, MESH_MEM);
    while (!(MEM_STATUS & MEM_WRITES_IDLE))
        ;
    GU_INVAL = 1;
    dlog_str("textures: test patterns\n");
    RS_CTRL = 1;                    /* the texture cache forgets what it held */
    texdemo_hw = hw_quad;
    texdemo_hw_piece = hw_piece;

    uint32_t drawn = 0;
    for (;;) {
        uint32_t st = wait_swap_done();
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
        VID_SWAP = 1;
        log_head("frames drawn: ", ++drawn);
    }
}
