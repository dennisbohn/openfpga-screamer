/*
 * The program (program.bin, data slot 2) in the SDRAM, started by the boot loader with its
 * results (docs/soc.md). With the game's original files in data slots 3-7 it builds track 1
 * from them (track_build.c) and puts the player's car on it (car.c): B accelerates, Y
 * brakes, left and right steer, L and R change gear on a manual car, X changes the view; the CPU picks and sorts the pieces (track.c), the geometry unit, setup unit and
 * rasterizer draw them. Without the files, the texture scene with test patterns, checked
 * once a run against the rasterizer's C model (raster.c), and a triangle moved with the
 * d-pad.
 * Every frame into the back buffer: palette, test band, background, the scene, the counts
 * and FPS; then a swap. After the third completed swap the results go to SYS_SIM once
 * (0x80001FFF when everything passed).
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
#include "track_build.h"
#include "car.h"
#include "hud.h"
#include "opp.h"
#include "efx.h"
#include "geom.h"

uint32_t frames_shown;             /* read by the trap handler */

#define STACK_FILL  0x57AC57ACu /* marks unused stack, for the high-water mark */
#define STACK_LOW   0x6000u     /* the stack may grow down to here (link.ld keeps the boot loader below) */

static uint32_t results;        /* SYS_SIM bits 20..0, from the boot loader */
static int textures;            /* textures in the SDRAM (the track's, or test patterns) */
static uint8_t col_bg = COL_BG, col_text = COL_TEXT, col_dim = COL_DIM, col_alert = COL_ALERT;

#define TEX_FILE    ((const volatile uint8_t *)(SDRAM_BASE + 0x2100000))  /* the track image, up to 7 MB */

/* The game's containers (data slots 3-7, core/data.json with the same addresses and limits)
   above the track image, in struct track_files' order; the build's scratch after them, up
   to the program */
#define GAME_SLOT   3
#define IMAGE_SLOT  30          /* the simulation only: the finished track image (load_image) */
static const struct game_file { const char *name; uint32_t addr, max; } game_files[5] = {
    { "MAP1H.DAT", SDRAM_BASE + 0x2800000, 0x400000 },
    { "GAME.DAT",  SDRAM_BASE + 0x2C00000, 0x100000 },
    { "MAP1.DAT",  SDRAM_BASE + 0x2D00000, 0x40000 },
    { "GEN.DAT",   SDRAM_BASE + 0x2D40000, 0x10000 },
    { "LOCAL.DAT", SDRAM_BASE + 0x2D50000, 0x10000 },
};
#define BUILD_SCRATCH   (SDRAM_BASE + 0x2D60000)
static struct track_files game;         /* where they are, as loaded */
/* the effects through MAPn.SMG (efx.h): 64 KB right under the track image, so that the radar's
   material at its end lies within a face's reach (32 KB) of the image's MLB */
#define EFX_SIZE    0x10000
#define EFX_MEM     ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE))
/* the cockpit's dashboard as blitter boxes (hud.h), 64 KB under the effects; unpacked through
   the build's scratch, which is free by then */
#define DASH_SIZE   0x10000
#define DASH_MEM    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - DASH_SIZE))
#define MAX_OBJECTS 32          /* the player's car, the opponents and their shadows, the player's
                                   shadow and smoke (track.c draws up to 32) */

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
static int back_wait;                   /* the backdrop is with the blitter: the first piece waits */
static uint32_t back_clocks, back_cycles;   /* the blitter's clocks for it, the CPU's wait */

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

/* a piece into the geometry unit's buffer: its place in view space and its mesh; a word
   waits only while the buffer is full */
static void hw_piece(const struct gu_piece *g)
{
    uint32_t t0 = rdcycle();
    if (back_wait) {                    /* the scene goes over the backdrop */
        while (RS_DRAW & RS_BUSY)
            ;
        back_clocks = BL_CLOCKS;
        back_cycles = rdcycle() - t0;
        back_wait = 0;
        t0 = rdcycle();
    }
    for (int k = 0; k < 9; k++) GU_PUSH = (uint32_t)g->m[k];
    for (int k = 0; k < 3; k++) GU_PUSH = (uint32_t)g->t[k];
    for (int k = 0; k < 3; k++) GU_PUSH = (uint32_t)g->e[k];
    GU_PUSH = (uint32_t)((uintptr_t)g->obj - SDRAM_BASE);
    GU_PUSH = (uint32_t)((uintptr_t)g->blk - SDRAM_BASE);
    GU_PUSH = ((uint32_t)g->row & 7) | (uint32_t)(g->mirror != 0) << 3 | ((uint32_t)g->row & 8) << 1 |
              (g->phase & 0x3FFF) << 16;
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
static const struct rs_clip track_clip = { 0, 0, FB_W, FB_H };     /* the whole screen, as the game's views */
static struct blit_list hud;            /* this frame's race screen, for the blitter (and the check) */
static struct blit_list back;           /* this frame's backdrop, the same */
static struct blit_list dashboard;      /* the cockpit's dashboard, the same */

/* the blitter's commands (blit.v): a copy's first word is its SDRAM byte address */
static void hw_blit(const struct blit_list *l)
{
    for (int i = 0; i < l->n; i++) {
        const uint32_t *c = l->c[i];
        uint32_t kind = c[1] >> 28 & 3;
        BL_PUSH = kind == BLIT_COPY || kind == BLIT_FONT ? (uint32_t)((uintptr_t)l->src[i] - SDRAM_BASE) : c[0];
        BL_PUSH = c[1];
        BL_PUSH = c[2];
        BL_PUSH = c[3];
    }
}

/* Once per run: the frame just drawn against the C models of the setup unit and the
   rasterizer, drawn into the SDRAM in the same order (the 3D view in clip, the dashboard ds
   if there is one, the radar's window, the race screen); the whole screen compared pixel by
   pixel */
static void track_check(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam,
                        struct track_object *objs, int nobj, const struct blit_list *bk,
                        const struct blit_list *ds, const struct blit_list *bl)
{
    volatile uint8_t *m = CHECK_BUF;
    struct track_stats ts;
    uint32_t differ = 0, first = 0, t0 = rdcycle();
    blit_model(m, bk);
    track_mode = TRACK_MODEL;
    track_draw_cam(m, clip, cam, objs, nobj, &ts);
    if (ds) blit_model(m, ds);
    struct su_view v;
    struct gu_piece radar;
    struct gu_stats gst = { 0, 0, 0, 0 };
    track_view(clip, &v);
    if (efx_radar_piece(&v, &radar)) gu_draw(m, &track_clip, &v, track_mlb(), &radar, &gst);
    blit_model(m, bl);
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

static int dashboard_init(void);

static void __attribute__((noreturn)) run_track(void)
{
    const uint32_t *pal = track_palette();
    uint32_t drawn = 0, keys_before = 0, last_frame = 0, steps = 0;
    uint32_t fps = 0, fps_drawn = 0, fps_frame = 0;
    int car_v = CAR_VIEW_START;
    car_init(0);
    opp_init(0);
    car_view(car_v);
    int dash_ok = dashboard_init();
    track_mode = TRACK_HW;
    track_hw_piece = hw_piece;
    GU_MLB = (uint32_t)((uintptr_t)track_mlb() - SDRAM_BASE);
    GU_INVAL = 1;
    uint32_t frame_cycles = 0;                  /* the last frame's, from the swap to the next */
    for (;;) {
        uint32_t st = wait_swap_done(), keys = IN_KEY, t_frame = rdcycle();
        frames_shown = drawn;
        if (drawn == 3) {
            SYS_SIM = SIM_DONE | results;
            dlog_str("stack used ");
            dlog_dec((int32_t)stack_used());
            dlog_str(" bytes\nSYS_SIM=");
            dlog_hex(SIM_DONE | results);
            dlog_char('\n');
        }
        /* the physics at 50 steps a second: one per video frame since the last picture */
        uint32_t n = drawn ? (VID_FRAME(st) - last_frame) & 0xFFFF : 1;
        last_frame = VID_FRAME(st);
        if (n > 4) n = 4;
        if ((keys & ~keys_before) & KEY_X) car_view(++car_v);
        efx_cyc_objects = efx_cyc_radar = 0;
        uint32_t t_phys = rdcycle();
        for (uint32_t k = 0; k < n; k++) {
            car_step(keys, keys_before);
            opp_step();
            keys_before = keys;
        }
        t_phys = rdcycle() - t_phys;
        steps += n;
        track_clock(n);
        const struct car_state *cs = car_state();
        struct track_cam cam;
        static struct track_object objs[MAX_OBJECTS];
        car_camera(&cam);
        int nobj = car_object(&cam, &objs[0]);
        nobj += opp_objects(objs + nobj, MAX_OBJECTS - nobj);
        nobj += car_effects(&cam, objs + nobj, MAX_OBJECTS - nobj);
        /* the 3D view: the whole screen, or in the cockpit its first 132 lines, the eye line
           85 down (track_view), the dashboard over the rest (without one the whole screen) */
        int cockpit = dash_ok && car_cockpit();
        const struct rs_clip clip = { 0, 0, FB_W, cockpit ? car_view_lines() : FB_H };
        struct su_view view;
        track_view(&clip, &view);

        /* the backdrop (the game's 0x83a68): the blitter copies it while the CPU culls the
           scene; the first piece for the geometry unit waits until it is done (hw_piece) */
        volatile uint8_t *fb = FB(!(st & VID_FRONT));
        draw_buf = !(st & VID_FRONT);
        blit_start(&back, (int)draw_buf);
        track_backdrop_blit(&back, &clip, &cam);
        BL_CLOCKS = 0;
        back_clocks = back_cycles = 0;
        back_wait = 1;
        hw_blit(&back);
        for (int i = 0; i < 256; i++) VID_PAL[i] = pal[i];

        /* the pieces from the CPU to the geometry unit, its faces to the setup unit, the
           triangles to the rasterizer */
        struct track_stats ts;
        uint32_t t0 = rdcycle();
        RS_PIXELS = 0;
        RS_MISSES = 0;
        RS_CLOCKS = 0;
        SU_CLOCKS = 0;
        GU_CLOCKS = 0;
        GU_FACES = 0;
        GU_MODE = draw_buf << 8;
        draw_wait = 0;
        RS_CLIP0 = RS_XY(clip.x0, clip.y0);
        RS_CLIP1 = RS_XY(clip.x1, clip.y1);
        su_view_regs(&view);
        track_draw_cam(0, &clip, &cam, objs, nobj, &ts);
        /* the radar's darkened window over the scene (efx.h), then the race screen: the
           blitter draws it once the scene is done. In the cockpit the dashboard comes before
           the window, which darkens it too: the blitter draws it once the scene is done, the
           CPU makes the race screen's list meanwhile, then waits for both to clip the window
           to the whole screen */
        struct gu_piece radar;
        int has_radar = efx_radar_piece(&view, &radar);
        BL_CLOCKS = 0;
        if (cockpit) {
            hud_dash(&dashboard, (int)draw_buf);
            hw_blit(&dashboard);
            hud_build(&hud, (int)draw_buf, (int)fps, 1);
            while (RS_DRAW & RS_BUSY)
                ;
            RS_CLIP1 = RS_XY(track_clip.x1, track_clip.y1);
            if (has_radar) hw_piece(&radar);
        } else {
            if (has_radar) hw_piece(&radar);
            hud_build(&hud, (int)draw_buf, (int)fps, 0);
        }
        hw_blit(&hud);
        while (RS_DRAW & RS_BUSY)
            ;
        uint32_t blit = BL_CLOCKS;
        uint32_t cycles = rdcycle() - t0, raster = RS_CLOCKS, pixels = RS_PIXELS, misses = RS_MISSES;
        uint32_t setup = SU_CLOCKS, geom = GU_CLOCKS;
        uint32_t r_miss = RS_MISS_CLOCKS, r_edge = RS_EDGE_CLOCKS, r_row = RS_ROW_CLOCKS;
        uint32_t r_drain = RS_DRAIN_CLOCKS, r_starved = RS_STARVED;
        ts.faces = GU_FACES;
        uint32_t efx_objects = efx_cyc_objects, efx_radar = efx_cyc_radar;    /* (the check adds) */
        if (drawn == 2) track_check(fb, &clip, &cam, objs, nobj, &back, cockpit ? &dashboard : 0, &hud);

        if (((VID_FRAME(st) - fps_frame) & 0xFFFF) >= VID_HZ) {
            fps = drawn - fps_drawn;
            fps_drawn = drawn;
            fps_frame = VID_FRAME(st);
        }
        if (drawn % 250 == 2) {
            dlog_str("track frame ");
            dlog_dec((int32_t)drawn);
            dlog_str(": car ");
            dlog_dec((int32_t)cs->x);
            dlog_char(' ');
            dlog_dec((int32_t)cs->y);
            dlog_char(' ');
            dlog_dec((int32_t)cs->z);
            dlog_str(", ");
            dlog_dec((int32_t)drive_kmh(cs));
            dlog_str(" km/h, gear ");
            dlog_dec(cs->gear);
            dlog_str(", walls ");
            dlog_dec((int32_t)car_wall_hits());
            dlog_str(", crashes ");
            dlog_dec((int32_t)car_crashes());
            dlog_str(", marker ");
            dlog_dec(car_marker());
            dlog_str(", place ");
            dlog_dec(opp_player_rank());
            dlog_str(" of ");
            dlog_dec(opp_count() + 1);
            dlog_str(", cars in view ");
            dlog_dec((int32_t)ts.objects);
            dlog_str(" slip x100 ");
            dlog_dec((int32_t)(cs->surface * 100.0f));
            dlog_str(", steps ");
            dlog_dec((int32_t)steps);
            dlog_str(" (");
            dlog_dec((int32_t)n);
            dlog_str(" now, ");
            dlog_dec((int32_t)t_phys);
            dlog_str(" cycles)");
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
            dlog_str(", blit ");
            dlog_dec((int32_t)blit);
            dlog_str(", backdrop ");
            dlog_dec((int32_t)back_clocks);
            dlog_str(" (wait ");
            dlog_dec((int32_t)back_cycles);
            dlog_char(')');
            dlog_str(", fps ");
            dlog_dec((int32_t)fps);
            dlog_str(", frame ");
            dlog_dec((int32_t)frame_cycles);
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
            float shx, shy, shh;
            if (car_shadow_at(&shx, &shy, &shh)) {
                dlog_str(", shadow ");
                dlog_dec((int32_t)shx);
                dlog_char(' ');
                dlog_dec((int32_t)shy);
                dlog_char(' ');
                dlog_dec((int32_t)shh);
            }
            dlog_str(", puffs ");
            dlog_dec(car_puffs());
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
            dlog_str(", effects ");                     /* shadows and smoke, radar (efx.h) */
            dlog_dec((int32_t)efx_objects);
            dlog_str(" + ");
            dlog_dec((int32_t)efx_radar);
            dlog_str(" (");
            dlog_dec((int32_t)efx_radar_lines);
            dlog_str(" lines)");
            dlog_str("\n  raster: miss ");
            dlog_dec((int32_t)r_miss);
            dlog_str(", edges ");
            dlog_dec((int32_t)r_edge);
            dlog_str(", rows ");
            dlog_dec((int32_t)r_row);
            dlog_str(", drain ");
            dlog_dec((int32_t)r_drain);
            dlog_str(", starved ");
            dlog_dec((int32_t)r_starved);
            dlog_str("\n");
        }
        frame_cycles = rdcycle() - t_frame;
        VID_SWAP = 1;
        log_head("frames drawn: ", ++drawn);
    }
}

/* Data slot id's file whole to addr (at most max bytes): its size from the slot table, 0
   if it has none, is too big or a read failed (*err the APF's result). The reads stay
   inside the file: its whole words, then the 1-3 bytes after them as the rest of the file. */
static uint32_t load_file(uint32_t id, uint32_t addr, uint32_t max, int *err)
{
    uint32_t size = slot_size(id), body = size & ~3u;
    *err = 0;
    if (size == 0 || size > max) return 0;
    if (body && (*err = slot_read(id, 0, addr, body)) != 0) return 0;
    if (size > body && (*err = slot_read(id, body, addr + body, SLOT_REST)) != 0) return 0;
    return size;
}

/* The five containers into the SDRAM; 1 if all are there */
static int load_game_files(void)
{
    const uint8_t *at[5];
    uint32_t size[5], t0 = rdcycle();
    for (int k = 0; k < 5; k++) {
        const struct game_file *g = &game_files[k];
        uint32_t id = GAME_SLOT + (uint32_t)k;
        int err;
        size[k] = load_file(id, g->addr, g->max, &err);
        at[k] = (const uint8_t *)(uintptr_t)g->addr;
        if (size[k]) continue;
        dlog_str(g->name);
        dlog_str(": not loaded (data slot ");
        dlog_dec((int32_t)id);
        dlog_str(", size ");
        dlog_dec((int32_t)slot_size(id));
        dlog_str(", result ");
        dlog_dec(err);
        dlog_str(")\n");
        return 0;
    }
    uint32_t ms = (rdcycle() - t0) / (CPU_HZ / 1000);
    game = (struct track_files){ at[0], at[1], at[2], at[3], at[4], size[0], size[1], size[2], size[3], size[4] };
    dlog_str("game files:");
    for (int k = 0; k < 5; k++) {
        dlog_char(' ');
        dlog_str(game_files[k].name);
        dlog_char(' ');
        dlog_dec((int32_t)size[k]);
    }
    dlog_str(" bytes loaded in ");
    dlog_dec((int32_t)ms);
    dlog_str(" ms\n");
    return 1;
}

/* The simulation's short cut (tools/simcheck.sh, SIM_IMAGE): the testbench offers the finished
   image as data slot IMAGE_SLOT, which core/data.json does not have, so on the Pocket its size
   is 0 and the image is built. The image build itself is checked by tools/check_track_build.sh. */
static int load_image(void)
{
    int err;
    uint32_t t0 = rdcycle(), size = load_file(IMAGE_SLOT, (uint32_t)(uintptr_t)TEX_FILE,
                                              game_files[0].addr - (uint32_t)(uintptr_t)TEX_FILE, &err);
    dcache_invalidate();
    if (!size || !track_open(TEX_FILE)) return 0;
    dlog_str("track image: ");
    dlog_dec((int32_t)size);
    dlog_str(" bytes from the testbench (data slot 30) in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return 1;
}

/* Track 1's image from them at TEX_FILE, its header opened; 1 if that worked */
static int build_track(void)
{
    struct track_build_info info;
    uint8_t *out = (uint8_t *)(uintptr_t)TEX_FILE;
    uint32_t size = track_build(&game, 1, out, game_files[0].addr - (uint32_t)(uintptr_t)out,
                                (uint8_t *)BUILD_SCRATCH, PROG_BASE - BUILD_SCRATCH, &info);
    while (!(MEM_STATUS & MEM_WRITES_IDLE))     /* all of it in the SDRAM for the 3D units */
        ;
    dcache_invalidate();                        /* the block check reads the SDRAM */
    if (!size || !track_open(TEX_FILE)) {
        dlog_str("track image: not built, stopped at ");
        dlog_str(size ? "its header" : info.fail);
        dlog_char('\n');
        return 0;
    }
    dlog_str("track image: ");
    dlog_dec((int32_t)size);
    dlog_str(" bytes built in ");
    dlog_dec((int32_t)(info.cycles / (CPU_HZ / 1000)));
    dlog_str(" ms, ");
    dlog_dec((int32_t)info.cycles);
    dlog_str(" cycles (unpacking ");
    dlog_dec((int32_t)info.unpack_cycles);
    dlog_str(")\n");
    return 1;
}

/* The effects through MAPn.SMG (efx.h) from the GFX container; after the testbench's image
   (load_image) that container alone is loaded for them */
static void effects(int image)
{
    uint32_t t0 = rdcycle();
    if (image) {
        int err;
        game.gfx = (const uint8_t *)(uintptr_t)game_files[0].addr;
        game.gfx_size = load_file(GAME_SLOT, game_files[0].addr, game_files[0].max, &err);
        dcache_invalidate();
    }
    int ok = game.gfx_size &&
             efx_init(game.gfx, game.gfx_size, 1, track_mlb(), track_palette(), EFX_MEM, EFX_SIZE);
    dlog_str("effects: MAP1.SMG ");
    dlog_str(efx_table() ? "ok" : "missing");
    dlog_str(", SHADOW.3DX ");
    dlog_str(efx_shadow() ? "ok" : "missing");
    dlog_str(", SMOKE.3DX ");
    dlog_str(efx_smoke() ? "ok" : "missing");
    dlog_str(ok ? ", in " : " - not all there, in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
}

/* The player's dashboard for the cockpit (hud.h) from the GFX container GAME.DAT and GEN.DAT;
   after the testbench's image (load_image) these two are loaded for it. 1 if it is there. */
static int dashboard_init(void)
{
    uint32_t t0 = rdcycle();
    if (!game.game_size) {
        int err;
        game.game = (const uint8_t *)(uintptr_t)game_files[1].addr;
        game.game_size = load_file(GAME_SLOT + 1, game_files[1].addr, game_files[1].max, &err);
        game.gen = (const uint8_t *)(uintptr_t)game_files[3].addr;
        game.gen_size = load_file(GAME_SLOT + 3, game_files[3].addr, game_files[3].max, &err);
        dcache_invalidate();
    }
    const struct car_spec *sp = &car_state()->spec;
    int ok = game.game_size && game.gen_size &&
             hud_cockpit_init(game.game, game.game_size, game.gen, game.gen_size, sp, DASH_MEM, DASH_SIZE,
                              (uint8_t *)(uintptr_t)BUILD_SCRATCH, PROG_BASE - BUILD_SCRATCH);
    dlog_str("cockpit: the dashboard of ");
    dlog_str(sp->name);
    dlog_str(ok ? " ok, in " : " missing (the cockpit shows the whole 3D view), in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return ok;
}

void prog_main(uint32_t boot_results, uint32_t log_pos, uint32_t log_cur)
{
    results = boot_results;
    stack_fill();
    dlog_attach(log_pos, log_cur);
    log_head("frames drawn: ", 0);
    dlog_str("program.bin: running in the SDRAM\n");

    /* the game's files (data slots 3-7, deferload) and the track image built from them at
       TEX_FILE; it is only used once all its blocks are as written */
    int placements = 0, image = 0;
    if ((image = load_image()) || (load_game_files() && build_track())) {
        dlog_str("track image: CRC of the 256 KB blocks:");
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
        dlog_str(" path points\n");
        effects(image);
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
