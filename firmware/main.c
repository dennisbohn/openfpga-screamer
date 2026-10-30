/*
 * The program (program.bin, data slot 2) in the SDRAM, started by the boot loader with its
 * results (docs/soc.md). With the game's original files in data slots 3-20 it shows the track
 * screen (menu.c: left and right choose, B or START take the track), builds the track chosen
 * from them (track_build.c) and puts the player's car on it (car.c): B accelerates, Y
 * brakes, left and right steer, L and R change gear on a manual car, X changes the view; the CPU picks and sorts the pieces (track.c), the geometry unit, setup unit and
 * rasterizer draw them. SELECT, or the race over for 5 s, goes back to the track screen.
 * Without the files, the texture scene with test patterns, checked
 * once a run against the rasterizer's C model (raster.c), and a triangle moved with the
 * d-pad.
 * Every frame into the back buffer: palette, test band, background, the scene, the counts
 * and FPS; then a swap. After the third completed swap the results go to SYS_SIM once
 * (0x80001FFF when everything passed); in the simulation's race (RACE_SLOT) after the swap of
 * the frame it names.
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
#include "scenery.h"
#include "geom.h"
#include "sound.h"
#include "music.h"
#include "menu.h"
#include "dat.h"

uint32_t frames_shown;             /* read by the trap handler */

#define STACK_FILL  0x57AC57ACu /* marks unused stack, for the high-water mark */
#define STACK_LOW   0x6000u     /* the stack may grow down to here (link.ld keeps the boot loader below) */

static uint32_t results;        /* SYS_SIM bits 20..0, from the boot loader */
static int textures;            /* textures in the SDRAM (the track's, or test patterns) */
static uint8_t col_bg = COL_BG, col_text = COL_TEXT, col_dim = COL_DIM, col_alert = COL_ALERT;

#define TEX_FILE    ((const volatile uint8_t *)(SDRAM_BASE + 0x2100000))  /* the track image, up to 7 MB */

/* The game's containers (core/data.json with the same addresses and limits) above the track
   image, in struct track_files' order: the track's GFX and DATA containers (data slots 3 and 5
   for track 1, 9-13 and 14-18 for tracks 2-6), each track's at the same place, and GAME.DAT,
   GEN.DAT, LOCAL.DAT (slots 4, 6, 7), loaded once; the build's scratch after them, up to the
   program */
#define GAME_SLOT   3
#define TRACK_FILES 5u          /* containers 0 and 2 (bits): the track's */
#define FIXED_FILES 0x1Au       /* 1, 3, 4: the ones that stay */
#define IMAGE_SLOT  30          /* the simulation only: the finished track image (load_image) */
#define RACE_SLOT   29          /* the simulation only: its size, the frames after the green light
                                   (run_track) */
#define TRACK_SLOT  28          /* the simulation only: its size, the track raced (none: track 1) */
#define MENU_CHECK_SLOT 27      /* the simulation only: there, the track screen instead (track_screen) */
static const struct game_file { const char *name; uint32_t addr, max; } game_files[5] = {
    { "MAP#H.DAT", SDRAM_BASE + 0x2800000, 0x400000 },     /* # the track's number */
    { "GAME.DAT",  SDRAM_BASE + 0x2C00000, 0x100000 },
    { "MAP#.DAT",  SDRAM_BASE + 0x2D00000, 0x40000 },
    { "GEN.DAT",   SDRAM_BASE + 0x2D40000, 0x10000 },
    { "LOCAL.DAT", SDRAM_BASE + 0x2D50000, 0x10000 },
};
#define BUILD_SCRATCH   (SDRAM_BASE + 0x2D60000)
static struct track_files game;         /* where they are, as loaded */
static int track_loaded;                /* the track whose two containers are loaded, 0 none */
/* The track screen's files (menu.h): of GFX/MENU.DAT and GFX/GRAF.DAT (data slots 19 and 20,
   core/data.json with the same addresses and limits) only the parts it needs, each at its place
   in the file; its picture, sprites and fonts after them. Below the sounds, where nothing else
   writes: loaded once. */
#define MENU_SLOT   19
#define GRAF_SLOT   20
#define MENU_FILE   (SDRAM_BASE + 0x200000)
#define GRAF_FILE   (SDRAM_BASE + 0x600000)
#define MENU_MAX    0x400000
#define MENU_MEM    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0xA00000))
#define MENU_SIZE   0x80000
/* the effects through MAPn.SMG (efx.h): 64 KB right under the track image, so that the radar's
   material at its end lies within a face's reach (32 KB) of the image's MLB */
#define EFX_SIZE    0x10000
#define EFX_MEM     ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE))
/* the cockpit's dashboard as blitter boxes (hud.h), 64 KB under the effects; unpacked through
   the build's scratch, which is free by then */
#define DASH_SIZE   0x10000
#define DASH_MEM    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - DASH_SIZE))
/* the props and moving scenery (scenery.h): their meshes, 64 KB under the dashboard */
#define SCENERY_SIZE 0x10000
#define SCENERY_MEM ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - DASH_SIZE - SCENERY_SIZE))
/* the sound effects' container SOUND.DAT (data slot 8, core/data.json with the same address and
   limit), 2 MB under the props below the track image; the mixer plays its samples from there */
#define SOUND_SLOT  8
#define SOUND_MEM   (SDRAM_BASE + 0x1C00000)
#define SOUND_MAX   0x200000
#define MAX_OBJECTS 64          /* the player's car, the opponents and their shadows, the player's
                                   shadow and smoke, the props and vehicles in reach (track.c draws up to 64) */

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

/* The frame's timeline (tools/make_timeline_svg.py draws it): at each mark the cycles since
   the frame began, the busy clocks of the geometry unit, setup unit, rasterizer and blitter
   and the CPU's cycles writing to them, each as its counter holds it (cleared during the
   frame; a mark follows every clear). Two buffers: the last whole frame is logged. */
enum { TL_START, TL_PHYS, TL_PREP, TL_CULL, TL_PIECES, TL_SCENE, TL_CLEAR, TL_HUD, TL_WAIT, TL_END, TL_MUSIC };
#define TL_MAX 48
static struct tl_mark { uint32_t t, gu, su, rs, bl, wait, tag; } tl[2][TL_MAX];
static uint32_t tl_n[2], tl_cur, tl_t0, tl_pieces, tl_scene;

static void tl_mark(uint32_t tag)
{
    uint32_t n = tl_n[tl_cur];
    if (n < TL_MAX) {
        tl[tl_cur][n] = (struct tl_mark){ rdcycle() - tl_t0, GU_CLOCKS, SU_CLOCKS, RS_CLOCKS, BL_CLOCKS,
                                          draw_wait, tag };
        tl_n[tl_cur] = n + 1;
    }
}

static void tl_begin(uint32_t t_frame)
{
    tl_cur ^= 1;
    tl_t0 = t_frame;
    tl_pieces = 0;
    tl[tl_cur][0] = (struct tl_mark){ 0, 0, 0, 0, 0, 0, TL_START };    /* the units idle: the last frame waited */
    tl_n[tl_cur] = 1;
}

static void tl_log(void)
{
    const uint32_t c = tl_cur ^ 1;
    dlog_str("  timeline (t gu su rs bl wait tag):");
    for (uint32_t i = 0; i < tl_n[c]; i++) {
        const struct tl_mark *m = &tl[c][i];
        const uint32_t v[7] = { m->t, m->gu, m->su, m->rs, m->bl, m->wait, m->tag };
        dlog_str(i % 6 ? " |" : "\n   ");
        for (int k = 0; k < 7; k++) {
            dlog_char(' ');
            dlog_dec((int32_t)v[k]);
        }
    }
    dlog_char('\n');
}

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
    if (tl_scene) {
        if (tl_pieces++ == 0)
            tl_mark(TL_CULL);
        else if (tl_pieces % 8 == 1)
            tl_mark(TL_PIECES);
    }
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

/* the whole screen fb against the models' picture m, pixel by pixel, into the log (the
   testbench looks for "track check: ok"); t0 when drawing m began */
static void compare_log(volatile uint8_t *fb, volatile uint8_t *m, uint32_t t0)
{
    uint32_t differ = 0, first = 0;
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
    uint32_t t0 = rdcycle();
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
    compare_log(fb, m, t0);
}

static int dashboard_init(void);

#define OVER_STEPS  250         /* the race over for 5 s: back to the track screen */

/* The race until SELECT or OVER_STEPS after its end (never in the simulation, which stops
   after the frame it checks) */
static void run_track(void)
{
    const uint32_t *pal = track_palette();
    uint32_t drawn = 0, keys_before = 0, last_frame = 0, steps = 0, over = 0;
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
    /* The simulation's race (tools/sim.sh, SIM_RACE_FRAMES; the keys SIM_KEYS holds): the
       testbench gives data slot RACE_SLOT, which core/data.json does not have, the size n; the
       start lights' countdown then runs at once before the first picture, and the n-th frame
       after the green light is checked and reported instead of the third. 0: as on the Pocket. */
    uint32_t sim_race = (SYS_FLAGS & FLAG_SIM) ? slot_size(RACE_SLOT) : 0;
    uint32_t check_at = 2 + sim_race;           /* the frame checked against the C models */
    for (;;) {
        uint32_t st = wait_swap_done(), keys = IN_KEY, t_frame = rdcycle();
        tl_begin(t_frame);
        music_frame();                          /* the ring kept filled (music.h) */
        tl_mark(TL_MUSIC);
        frames_shown = drawn;
        if (drawn == check_at + 1) {
            SYS_SIM = SIM_DONE | results;
            dlog_str("stack used ");
            dlog_dec((int32_t)stack_used());
            dlog_str(" bytes\nSYS_SIM=");
            dlog_hex(SIM_DONE | results);
            /* the checked frame's camera and car, to set against the original's
               (tools/dosram.py cam): places in units, angles in 1/10000 rad */
            struct track_cam c;
            const struct car_state *s = car_state();
            car_camera(&c);
            const float v[10] = { c.x, c.y, c.h, c.yaw * 1e4f, c.pitch * 1e4f, c.roll * 1e4f,
                                  s->x, s->y, s->z, s->yaw * 1e4f };
            dlog_str("\ncamera x y h yaw pitch roll, car x y z yaw:");
            for (int i = 0; i < 10; i++) {
                dlog_char(' ');
                dlog_dec((int32_t)v[i]);
            }
            dlog_char('\n');
        }
        /* the physics at 50 steps a second: one per video frame since the last picture */
        uint32_t n = drawn ? (VID_FRAME(st) - last_frame) & 0xFFFF : 1;
        last_frame = VID_FRAME(st);
        if (n > 4) n = 4;
        if ((keys & ~keys_before) & KEY_X) car_view(++car_v);
        if (((keys & ~keys_before) & KEY_SELECT) || over >= OVER_STEPS) return;
        efx_cyc_objects = efx_cyc_radar = 0;
        uint32_t t_phys = rdcycle();
        for (uint32_t k = 0; k < n; k++) {
            car_step(keys, keys_before);
            opp_step();
            scenery_step();
            keys_before = keys;
            if (sim_race && !car_race()->on && k == n - 1) n++;    /* the simulation's countdown at once */
            if (car_race()->finished || car_race()->game_over) over++;
        }
        t_phys = rdcycle() - t_phys;
        tl_mark(TL_PHYS);
        steps += n;
        track_clock(n);
        const struct car_state *cs = car_state();
        struct track_cam cam;
        static struct track_object objs[MAX_OBJECTS];
        car_camera(&cam);
        int nobj = car_object(&cam, &objs[0]);
        nobj += opp_objects(objs + nobj, MAX_OBJECTS - nobj);
        nobj += car_effects(&cam, objs + nobj, MAX_OBJECTS - nobj);
        nobj += scenery_objects(&cam, objs + nobj, MAX_OBJECTS - nobj);
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
        tl_mark(TL_PREP);
        RS_CLIP0 = RS_XY(clip.x0, clip.y0);
        RS_CLIP1 = RS_XY(clip.x1, clip.y1);
        su_view_regs(&view);
        tl_scene = 1;
        track_draw_cam(0, &clip, &cam, objs, nobj, &ts);
        tl_scene = 0;
        tl_mark(TL_SCENE);
        /* the radar's darkened window over the scene (efx.h), then the race screen: the
           blitter draws it once the scene is done. In the cockpit the dashboard comes before
           the window, which darkens it too: the blitter draws it once the scene is done, the
           CPU makes the race screen's list meanwhile, then waits for both to clip the window
           to the whole screen */
        struct gu_piece radar;
        int has_radar = efx_radar_piece(&view, &radar);
        BL_CLOCKS = 0;
        tl_mark(TL_CLEAR);
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
        tl_mark(TL_HUD);
        while (RS_DRAW & RS_BUSY)
            ;
        tl_mark(TL_WAIT);
        uint32_t blit = BL_CLOCKS;
        uint32_t cycles = rdcycle() - t0, raster = RS_CLOCKS, pixels = RS_PIXELS, misses = RS_MISSES;
        uint32_t setup = SU_CLOCKS, geom = GU_CLOCKS;
        uint32_t r_miss = RS_MISS_CLOCKS, r_edge = RS_EDGE_CLOCKS, r_row = RS_ROW_CLOCKS;
        uint32_t r_drain = RS_DRAIN_CLOCKS, r_starved = RS_STARVED;
        ts.faces = GU_FACES;
        uint32_t efx_objects = efx_cyc_objects, efx_radar = efx_cyc_radar;    /* (the check adds) */
        if (drawn == check_at) track_check(fb, &clip, &cam, objs, nobj, &back, cockpit ? &dashboard : 0, &hud);

        if (((VID_FRAME(st) - fps_frame) & 0xFFFF) >= VID_HZ) {
            fps = drawn - fps_drawn;
            fps_drawn = drawn;
            fps_frame = VID_FRAME(st);
        }
        if (drawn % 250 == 2 || drawn == check_at) {
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
            if (drawn == check_at) {                /* the log has 4 KB: once */
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
            dlog_str(", late ");                        /* a mixer pass late since the start */
            dlog_dec((int32_t)(AU_STATUS & AU_LATE));
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
            tl_log();
        }
        frame_cycles = rdcycle() - t_frame;
        tl_mark(TL_END);
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

/* name with '#' made track n's number into buf */
static const char *track_name(const char *name, int n, char buf[16])
{
    int i = 0;
    for (; name[i] && i < 15; i++)
        buf[i] = name[i] == '#' ? (char)('0' + n) : name[i];
    buf[i] = 0;
    return buf;
}

/* The containers of game_files whose bits are in which into the SDRAM, the track's those of
   track n (data slots: GAME_SLOT + k, MAP2H..MAP6H 9-13, MAP2..MAP6 14-18); 1 if all are there */
static int load_containers(int n, uint32_t which)
{
    const uint8_t **at[5] = { &game.gfx, &game.game, &game.data, &game.gen, &game.local };
    uint32_t *size[5] = { &game.gfx_size, &game.game_size, &game.data_size, &game.gen_size, &game.local_size };
    uint32_t t0 = rdcycle();
    char name[16];
    dlog_str("game files:");
    for (int k = 0; k < 5; k++) {
        if (!(which >> k & 1)) continue;
        const struct game_file *g = &game_files[k];
        uint32_t id = n > 1 && k == 0 ? 7u + (uint32_t)n : n > 1 && k == 2 ? 12u + (uint32_t)n : GAME_SLOT + (uint32_t)k;
        int err;
        *size[k] = load_file(id, g->addr, g->max, &err);
        *at[k] = (const uint8_t *)(uintptr_t)g->addr;
        dlog_char(' ');
        dlog_str(track_name(g->name, n, name));
        if (!*size[k]) {
            dlog_str(" not loaded (data slot ");
            dlog_dec((int32_t)id);
            dlog_str(", size ");
            dlog_dec((int32_t)slot_size(id));
            dlog_str(", result ");
            dlog_dec(err);
            dlog_str(")\n");
            return 0;
        }
        dlog_char(' ');
        dlog_dec((int32_t)*size[k]);
    }
    dlog_str(" bytes loaded in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return 1;
}

/* Bytes from .. to of data slot id's file (size bytes) to the same offsets from addr: the
   file's whole words there, then its last 1-3 bytes as the rest of the file if to reaches
   them. 0 if that worked, else the APF's result. */
static int load_range(uint32_t id, uint32_t size, uint32_t from, uint32_t to, uint32_t addr)
{
    uint32_t body = size & ~3u, a = from & ~3u, b = (to + 3) & ~3u;
    if (b > body) b = body;
    int err = a < b ? slot_read(id, a, addr + a, b - a) : 0;
    if (!err && to > body) err = slot_read(id, body, addr + body, SLOT_REST);
    return err;
}

/* Of data slot id's container (at most max bytes) its directory and the parts p names, each
   at its place from addr as if the whole file were there: its size, 0 if the file or a part is
   missing or a read failed */
static uint32_t load_parts(uint32_t id, uint32_t addr, uint32_t max, const struct menu_part *p)
{
    const uint8_t *f = (const uint8_t *)(uintptr_t)addr;
    uint32_t size = slot_size(id);
    if (size < 8 || size > max || load_range(id, size, size - 4, size, addr)) return 0;
    const uint8_t *q = f + size - 4;
    uint32_t dir = q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
    if (dir > size - 4 || load_range(id, size, size - 4 - dir, size - 4, addr)) return 0;
    for (; p->name; p++) {
        struct dat_entry e;
        if (!dat_find(f, size, p->name, &e) || e.offset > size || e.stored > size - e.offset) return 0;
        if (load_range(id, size, e.offset, e.offset + (e.stored < p->most ? e.stored : p->most), addr)) return 0;
    }
    return size;
}

/* The track screen from GFX/MENU.DAT, GFX/GRAF.DAT and DATA/LOCAL.DAT; 1 if it is there */
static int menus(void)
{
    uint32_t t0 = rdcycle();
    uint32_t menu_size = load_parts(MENU_SLOT, MENU_FILE, MENU_MAX, menu_dat_parts);
    uint32_t graf_size = load_parts(GRAF_SLOT, GRAF_FILE, MENU_MAX, menu_graf_parts);
    int ok = menu_size && graf_size &&
             menu_init((const uint8_t *)(uintptr_t)MENU_FILE, menu_size, (const uint8_t *)(uintptr_t)GRAF_FILE,
                       graf_size, game.local, game.local_size, MENU_MEM, MENU_SIZE, (uint8_t *)BUILD_SCRATCH,
                       PROG_BASE - BUILD_SCRATCH);
    while (!(MEM_STATUS & MEM_WRITES_IDLE))     /* the picture in the SDRAM for the blitter */
        ;
    dlog_str("track screen: MENU.DAT ");
    dlog_dec((int32_t)menu_size);
    dlog_str(", GRAF.DAT ");
    dlog_dec((int32_t)graf_size);
    dlog_str(ok ? " bytes, their parts loaded in " : " bytes - not all there (no track screen: track 1), in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return ok;
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

/* Track n's image from them at TEX_FILE, its header opened; 1 if that worked */
static int build_track(int n)
{
    struct track_build_info info;
    uint8_t *out = (uint8_t *)(uintptr_t)TEX_FILE;
    uint32_t size = track_build(&game, n, out, game_files[0].addr - (uint32_t)(uintptr_t)out,
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

/* Track n's effects through MAPn.SMG (efx.h) from its GFX container */
static void effects(int n)
{
    uint32_t t0 = rdcycle();
    char name[16];
    int ok = efx_init(game.gfx, game.gfx_size, n, track_mlb(), track_palette(), EFX_MEM, EFX_SIZE);
    dlog_str("effects: ");
    dlog_str(track_name("MAP#.SMG ", n, name));
    dlog_str(efx_table() ? "ok" : "missing");
    dlog_str(", SHADOW.3DX ");
    dlog_str(efx_shadow() ? "ok" : "missing");
    dlog_str(", SMOKE.3DX ");
    dlog_str(efx_smoke() ? "ok" : "missing");
    dlog_str(ok ? ", in " : " - not all there, in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
}

/* Track n's props and moving scenery (scenery.h) from its DATA and GFX containers */
static void scenery(int n)
{
    uint32_t t0 = rdcycle();
    int props, moving;
    scenery_init(game.data, game.data_size, game.gfx, game.gfx_size, n, SCENERY_MEM, SCENERY_SIZE, &props,
                 &moving);
    dlog_str("scenery: ");
    dlog_dec(props);
    dlog_str(" props, ");
    dlog_dec(moving);
    dlog_str(" moving, in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
}

/* The sound effects (sound.h): SOUND.DAT whole into the SDRAM once, their list from GEN.DAT
   before each race (the scenery adds the track's own after them) */
static void sounds(void)
{
    static uint32_t size;
    uint32_t t0 = rdcycle();
    int err;
    if (!size) size = load_file(SOUND_SLOT, SOUND_MEM, SOUND_MAX, &err);
    dcache_invalidate();
    int n = size && game.gen_size ? snd_init((const uint8_t *)(uintptr_t)SOUND_MEM, size, game.gen, game.gen_size) : 0;
    AU_STATUS = 0;
    dlog_str("sound: ");
    dlog_dec(n);
    dlog_str(" effects, ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
}

/* The player's dashboard for the cockpit (hud.h) from the GFX container GAME.DAT and GEN.DAT;
   1 if it is there */
static int dashboard_init(void)
{
    uint32_t t0 = rdcycle();
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

/* The track screen (menu.h) from track n on until one is taken: its number. The picture by the
   blitter, the models through the geometry unit while it waits (hw_piece), the texts after them.
   In the simulation (SIM_MENU) its third frame is checked against the C models and reported. */
static int track_screen(int n)
{
    const uint32_t *pal = menu_palette();
    uint32_t drawn = 0, keys_before = IN_KEY, last_frame = 0, sim = SYS_FLAGS & FLAG_SIM;
    menu_enter(n);
    music_play(0);
    track_mode = TRACK_HW;
    track_hw_piece = hw_piece;
    GU_MLB = (uint32_t)((uintptr_t)menu_mlb() - SDRAM_BASE);
    GU_INVAL = 1;
    for (;;) {
        uint32_t st = wait_swap_done(), keys = IN_KEY;
        music_frame();
        frames_shown = drawn;
        if (sim && drawn == 3) {
            SYS_SIM = SIM_DONE | results;
            dlog_str("stack used ");
            dlog_dec((int32_t)stack_used());
            dlog_str(" bytes\n");
        }
        uint32_t steps = drawn ? (VID_FRAME(st) - last_frame) & 0xFFFF : 1;
        last_frame = VID_FRAME(st);
        if (steps > 4) steps = 4;
        for (uint32_t k = 0; k < steps; k++) {
            int chosen = menu_step(keys, keys_before);
            keys_before = keys;
            if (chosen) return chosen;
        }
        draw_buf = !(st & VID_FRONT);
        blit_start(&back, (int)draw_buf);
        menu_backdrop(&back);
        back_wait = 1;
        hw_blit(&back);
        for (int i = 0; i < 256; i++) VID_PAL[i] = pal[i];
        GU_MODE = draw_buf << 8;
        RS_CLIP0 = RS_XY(track_clip.x0, track_clip.y0);
        RS_CLIP1 = RS_XY(track_clip.x1, track_clip.y1);
        struct su_view view;
        menu_view(&view);
        su_view_regs(&view);
        menu_scene(0);
        back_wait = 0;
        blit_start(&hud, (int)draw_buf);
        menu_text(&hud);
        hw_blit(&hud);
        while (RS_DRAW & RS_BUSY)
            ;
        if (sim && drawn == 2) {
            volatile uint8_t *m = CHECK_BUF;
            uint32_t t0 = rdcycle();
            blit_model(m, &back);
            track_mode = TRACK_MODEL;
            menu_scene(m);
            track_mode = TRACK_HW;
            blit_model(m, &hud);
            compare_log(FB(draw_buf), m, t0);
        }
        VID_SWAP = 1;
        drawn++;
    }
}

/* Track n from its containers: the image built at TEX_FILE (or the testbench's) and used only
   once all its blocks are as written, the effects and scenery, then the race until it is left;
   0 if the track could not be set up */
static int race(int n)
{
    int placements = 0;
    music_stop();                               /* before the loads: none of its reads running */
    if (track_loaded != n) {
        track_loaded = 0;
        if (!load_containers(n, TRACK_FILES)) return 0;
        track_loaded = n;
    }
    if (load_image() || build_track(n)) {
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
    if (!placements) return 0;
    const volatile uint32_t *pal = (const volatile uint32_t *)track_palette();
    col_bg = nearest(pal, 0, 0, 0);
    col_text = nearest(pal, 255, 255, 255);
    col_dim = nearest(pal, 120, 200, 120);
    col_alert = nearest(pal, 230, 25, 178);
    dlog_str("track ");
    dlog_dec(n);
    dlog_str(": ");
    dlog_dec(placements);
    dlog_str(" placements, ");
    dlog_dec((int32_t)track_path_length());
    dlog_str(" path points\n");
    effects(n);
    sounds();                   /* before the scenery, which adds the track's own sounds */
    scenery(n);
    RS_CTRL = 1;
    music_play(n);
    run_track();
    AU_STOP = 0xFFFFFFFFu;                      /* the race's sounds end with it */
    return 1;
}

void prog_main(uint32_t boot_results, uint32_t log_pos, uint32_t log_cur)
{
    results = boot_results;
    stack_fill();
    dlog_attach(log_pos, log_cur);
    log_head("frames drawn: ", 0);
    dlog_str("program.bin: running in the SDRAM\n");

    /* the game's files that stay (data slots 4, 6, 7, deferload) and the track screen's; then
       the track screen and the race on the track chosen, for ever. The simulation races the
       testbench's track at once. */
    if (load_containers(0, FIXED_FILES)) {
        music_init(game.gen, game.gen_size);
        int sim = (SYS_FLAGS & FLAG_SIM) != 0, screen = (!sim || slot_size(MENU_CHECK_SLOT)) && menus();
        int n = sim && slot_size(TRACK_SLOT) ? (int)slot_size(TRACK_SLOT) : 1;
        for (;;) {
            if (screen) n = track_screen(n);
            if (!race(n) && !screen) break;
        }
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
