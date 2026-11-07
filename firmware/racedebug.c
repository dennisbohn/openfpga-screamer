/*
 * The debug build's measuring and logging of a race (racedebug.h): the timeline of a picture, the figures of
 * the units, the frame time statistics and overlay, the status block and the first line of debug.txt, the
 * marks and the free camera, and the benchmark. It is compiled only with DEBUG=1.
 */
#include "racedebug.h"
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "screen.h"
#include "ftime.h"
#include "mark.h"
#include "bench.h"
#include "config.h"
#include "car.h"
#include "drive.h"
#include "opp.h"
#include "efx.h"
#include "menu.h"
#include "speech.h"
#include "track_build.h"

struct race_debug race_dbg;

static int pocket_race_logged;          /* the status block went to debug.txt, on the Pocket */

/*
 * The frame timeline (tools/make_timeline_svg.py draws it from the log).
 * At each mark in a frame one record is stored:
 *     t     CPU cycles since the frame began
 *     gu, su, rs, bl   the busy clocks of the geometry unit, setup unit, rasterizer and blitter, as their
 *           counters hold them (the frame clears the counters at some points and sets a mark after each
 *           clear)
 *     wait  CPU cycles spent so far on writing pieces to the geometry unit (draw_wait)
 *     tag   which mark it is (TL_*)
 * Two buffers alternate. One is filled by the frame that runs; the last whole frame, in the other, is logged.
 */
#define TL_MAX 48
static struct tl_mark { uint32_t t, gu, su, rs, bl, wait, tag; } tl[2][TL_MAX];
static uint32_t tl_n[2], tl_cur, tl_t0;

/* Stores a record with this tag, if the buffer has room. */
void tl_mark(uint32_t tag)
{
    uint32_t n = tl_n[tl_cur];
    if (n < TL_MAX) {
        tl[tl_cur][n] = (struct tl_mark){ rdcycle() - tl_t0, GU_CLOCKS, SU_CLOCKS, RS_CLOCKS, BL_CLOCKS,
                                          race_dbg.draw_wait, tag };
        tl_n[tl_cur] = n + 1;
    }
}

/* Starts the timeline of a new frame in the other buffer. t_frame is the cycle count at the frame's start. */
static void tl_begin(uint32_t t_frame)
{
    tl_cur ^= 1;
    tl_t0 = t_frame;
    race_dbg.scene_pieces = 0;
    /* The first record is all zeros: the units are idle, because the last frame waited for them. */
    tl[tl_cur][0] = (struct tl_mark){ 0, 0, 0, 0, 0, 0, TL_START };
    tl_n[tl_cur] = 1;
}

/* Writes the last whole frame's records to debug.txt, six to a line. */
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

void log_race_head(uint32_t n)
{
    char buf[DLOG_HEAD + 64];
    ft_head(fmt_udec(fmt_str(fmt_str(buf, "Screamer Pocket step 3, "), "frames drawn: "), n));
    dlog_head(buf);
}

void race_debug_init(void)
{
    dlog_keep(MARK_ROOM);
}

/* The index of the entry of the 256-colour palette that is nearest to the colour (r, g, b), by the squared
   distance in RGB. */
static uint8_t nearest(const uint32_t *palette, int r, int g, int b)
{
    uint32_t best = ~0u;
    uint8_t n = 0;
    for (int i = 0; i < 256; i++) {
        int dr = (int)(palette[i] >> 16 & 255) - r, dg = (int)(palette[i] >> 8 & 255) - g,
            db = (int)(palette[i] & 255) - b;
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best) { best = d; n = (uint8_t)i; }
    }
    return n;
}

void race_debug_start(int track, int sim, uint32_t sim_race, uint32_t check_at, const uint32_t *palette)
{
    ft_reset();
    mark_reset();
    race_dbg = (struct race_debug){ .bench_frames = race_dbg.bench_frames, .track = track, .sim = sim,
                                    .sim_race = sim_race, .check_at = check_at,
                                    .fg = nearest(palette, 255, 255, 255), .bg = nearest(palette, 0, 0, 0) };
}

void race_debug_frame(uint32_t t_frame, uint32_t drawn)
{
    race_dbg.t_frame = t_frame;
    race_dbg.drawn = drawn;
    tl_begin(t_frame);
    if (drawn == race_dbg.check_at + 1) {
        dlog_str("stack used ");
        dlog_dec((int32_t)stack_used());
        dlog_str(" bytes\n");
    }
}

/* The key chords of the frame time overlay, the marks and the free camera. A with L and R switches the
   overlay on or off. Nothing else in the race uses A, but L and R change gears. While A is held they are held
   back (swallow), so the chord, pressed with A first, changes no gear. */
uint32_t race_debug_keys(uint32_t keys, uint32_t press, int view)
{
    struct race_debug *d = &race_dbg;
    const int chord = (keys & (KEY_A | KEY_L | KEY_R)) == (KEY_A | KEY_L | KEY_R);
    if (chord && !d->chord_before) ft_overlay_toggle();
    d->chord_before = chord;
    if (!d->bench_frames && (keys & KEY_A) && (press & KEY_X)) {      /* A held, X pressed: write a mark */
        d->mark_no = mark_save(d->track, d->free_cam ? 9 : view, d->drawn);    /* view 9 is the free camera */
        d->mark_show = 75;
    }
    if (!d->bench_frames && (keys & KEY_A) && (press & KEY_Y)) {      /* A held, Y pressed: free camera */
        d->free_cam = !d->free_cam;
        d->free_c = d->last_cam;                /* it starts where the car's camera is */
    }
    if (d->free_cam && (press & KEY_START)) {  /* START gives the car's camera back; it does not pause */
        d->free_cam = 0;
        press &= ~KEY_START;
    }
    if (keys & KEY_A) {
        d->swallow |= keys & (KEY_L | KEY_R);
        press &= ~KEY_X;                       /* A with X writes a mark instead of changing the view */
    }
    d->swallow &= keys;
    return press;
}

void race_debug_physics_begin(void)
{
    race_dbg.t_phys = rdcycle();
}

void race_debug_physics_end(uint32_t steps)
{
    race_dbg.t_phys = rdcycle() - race_dbg.t_phys;
    tl_mark(TL_PHYS);
    race_dbg.steps += steps;
}

/* With the free camera on, the player's controls move that camera. While A is held, X, Y, L and R do not move
   it, because with A they are the commands for marks, the free camera and the overlay. Every camera is
   recorded for the marks. */
void race_debug_camera(struct track_cam *cam, uint32_t keys)
{
    struct race_debug *d = &race_dbg;
    d->last_cam = *cam;
    if (d->free_cam) {
        free_cam_step(&d->free_c, (keys & KEY_A) ? keys & ~(KEY_X | KEY_Y | KEY_L | KEY_R) : keys);
        *cam = d->free_c;
    }
    if (!d->bench_frames) mark_push(cam);
}

void race_debug_backdrop(void)
{
    BL_CLOCKS = 0;
    race_dbg.back_clocks = race_dbg.back_cycles = 0;
}

void race_debug_scene_begin(void)
{
    struct race_debug *d = &race_dbg;
    d->t_scene = rdcycle();
    RS_PIXELS = 0;
    RS_MISSES = 0;
    RS_CLOCKS = 0;
    SU_CLOCKS = 0;
    GU_CLOCKS = 0;
    GU_FACES = 0;
    d->draw_wait = 0;
    d->t_pre = rdcycle() - d->t_frame;
    tl_mark(TL_PREP);
    d->in_scene = 1;
}

void race_debug_scene_end(void)
{
    race_dbg.in_scene = 0;
    tl_mark(TL_SCENE);
}

void race_debug_screen_begin(void)
{
    BL_CLOCKS = 0;
    tl_mark(TL_CLEAR);
}

void race_debug_screen_end(void)
{
    tl_mark(TL_HUD);
    race_dbg.t_wait = rdcycle();
}

/* Reads the counters of the units. The CPU cycles count from just before the scene up to the end of the
   rasterizer's work. The effects' cycles are copied before the simulation's check runs, because the check
   adds to those counters as well. */
void race_debug_measure(struct track_stats *ts)
{
    struct race_debug *d = &race_dbg;
    tl_mark(TL_WAIT);
    d->units.blit = BL_CLOCKS;
    d->units.cycles = rdcycle() - d->t_scene;
    d->units.raster = RS_CLOCKS;
    d->units.pixels = RS_PIXELS;
    d->units.misses = RS_MISSES;
    d->units.setup = SU_CLOCKS;
    d->units.geom = GU_CLOCKS;
    d->units.r_miss = RS_MISS_CLOCKS;
    d->units.r_edge = RS_EDGE_CLOCKS;
    d->units.r_row = RS_ROW_CLOCKS;
    d->units.r_drain = RS_DRAIN_CLOCKS;
    d->units.r_starved = RS_STARVED;
    ts->faces = GU_FACES;
    d->units.efx_objects = efx_cyc_objects;
    d->units.efx_radar = efx_cyc_radar;
    d->t_check = rdcycle();                     /* the check is timed, to take its time off the frame's */
}

/* Line 1 of the status block: the car, the race, the scene and the busy clocks of the units. */
static void log_track_frame(const struct track_stats *ts, uint32_t steps_now)
{
    const struct race_debug *d = &race_dbg;
    const struct car_state *cs = car_state();
    dlog_str("track frame ");
    dlog_dec((int32_t)d->drawn);
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
    dlog_dec((int32_t)ts->objects);
    dlog_str(" slip x100 ");
    dlog_dec((int32_t)(cs->surface * 100.0f));
    dlog_str(", steps ");
    dlog_dec((int32_t)d->steps);
    dlog_str(" (");
    dlog_dec((int32_t)steps_now);
    dlog_str(" now, ");
    dlog_dec((int32_t)d->t_phys);
    dlog_str(" cycles)");
    dlog_str(", ");
    dlog_dec((int32_t)ts->pieces);
    dlog_str(" pieces, ");
    dlog_dec((int32_t)ts->faces);
    dlog_str(" faces, ");
    dlog_dec((int32_t)d->units.pixels);
    dlog_str(" px, cpu ");
    dlog_dec((int32_t)d->units.cycles);
    dlog_str(", geometry ");
    dlog_dec((int32_t)d->units.geom);
    dlog_str(", setup ");
    dlog_dec((int32_t)d->units.setup);
    dlog_str(", raster ");
    dlog_dec((int32_t)d->units.raster);
    dlog_str(", misses ");
    dlog_dec((int32_t)d->units.misses);
    dlog_str(", blit ");
    dlog_dec((int32_t)d->units.blit);
    dlog_str(", backdrop ");                    /* the blitter's clocks, then the CPU's wait */
    dlog_dec((int32_t)d->back_clocks);
    dlog_str(" (wait ");
    dlog_dec((int32_t)d->back_cycles);
    dlog_char(')');
    dlog_str(", fps ");
    dlog_dec((int32_t)d->fps);
    dlog_str(", frame ");                       /* the cycles of the previous frame */
    dlog_dec((int32_t)d->frame_cycles);
    if (d->drawn == d->check_at) {              /* the camera is logged once, at the checked frame */
        dlog_str(", camera ");
        dlog_dec(ts->cam[0]);
        dlog_char(' ');
        dlog_dec(ts->cam[1]);
        dlog_char(' ');
        dlog_dec(ts->cam[2]);
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
    dlog_str(", late ");                        /* mixer passes that were late since the start */
    dlog_dec((int32_t)(AU_STATUS & AU_LATE));
}

/* Line 2 of the status block: the CPU cycles of the stages of the frame. */
static void log_cycles(const struct track_stats *ts)
{
    const struct race_debug *d = &race_dbg;
    dlog_str("\n  cycles: cull ");
    dlog_dec((int32_t)ts->cyc_cull);
    dlog_str(", pieces ");
    dlog_dec((int32_t)ts->cyc_piece);
    dlog_str(", out ");
    dlog_dec((int32_t)ts->cyc_out);
    dlog_str(" (wait ");
    dlog_dec((int32_t)d->draw_wait);
    dlog_str("), after ");
    dlog_dec((int32_t)(d->units.cycles - ts->cyc_cull - ts->cyc_piece - ts->cyc_out));
    dlog_str(", effects ");                     /* cycles: shadows and smoke + radar (efx.h) */
    dlog_dec((int32_t)d->units.efx_objects);
    dlog_str(" + ");
    dlog_dec((int32_t)d->units.efx_radar);
    dlog_str(" (");
    dlog_dec((int32_t)efx_radar_lines);
    dlog_str(" lines)");
}

/* Line 3 of the status block: the clocks the rasterizer spent on each kind of work. */
static void log_raster(void)
{
    const struct race_debug *d = &race_dbg;
    dlog_str("\n  raster: miss ");
    dlog_dec((int32_t)d->units.r_miss);
    dlog_str(", edges ");
    dlog_dec((int32_t)d->units.r_edge);
    dlog_str(", rows ");
    dlog_dec((int32_t)d->units.r_row);
    dlog_str(", drain ");
    dlog_dec((int32_t)d->units.r_drain);
    dlog_str(", starved ");
    dlog_dec((int32_t)d->units.r_starved);
    dlog_str("\n");
}

/* The frame's work goes to the statistics and the timing overlay (ftime.h). The overlay shows the frames up
   to the previous one. Not counted as frames of the race are the picture of the question and the first
   picture of the simulation's race and of the benchmark, in which the countdown of the start lights (300
   steps) runs at once. The time of the simulation's check is not part of the frame's work. */
static void frame_statistics(volatile uint8_t *fb, int ask, uint32_t t_check, const struct track_stats *ts)
{
    struct race_debug *d = &race_dbg;
    if (ask || ((d->sim_race || d->bench_frames) && d->drawn == 0)) return;
    if (ft_overlay_on()) ft_draw(fb, d->fg, d->bg, track_far_scale(), track_lod_scale());
    if (d->mark_show) {                         /* the mark's number, or a note that debug.txt was full */
        char t[20];
        if (d->mark_no) fmt_udec(fmt_str(t, "MARK "), (uint32_t)d->mark_no);
        else fmt_str(t, "MARK: LOG FULL");
        con_text(fb, 0, 3, t, d->fg, d->bg);
        d->mark_show--;
    }
    if (d->free_cam) con_text(fb, 0, 4, "FREE CAMERA - START: BACK", d->fg, d->bg);
    struct ft_frame ff = { rdcycle() - d->t_frame - t_check, d->t_pre, ts->cyc_cull,
                           ts->cyc_piece + ts->cyc_out, 0, rdcycle() - d->t_wait,
                           ts->faces, ts->pieces, (uint32_t)car_marker() };
    ff.post = d->t_wait - d->t_frame - d->t_pre - ts->cyc_cull - ts->cyc_piece - ts->cyc_out;
    ft_frame(&ff);
}

/* The status block for debug.txt. The 4 KB log has room for one big block only, so it is written at these
   frames: in the simulation at frame 2, then every 250 frames (not in a simulated race) and at the checked
   frame; on the Pocket once, at the checked frame; never in the benchmark. The block has three lines and
   the timeline of the last whole frame (tl_log). */
static int status_due(void)
{
    const struct race_debug *d = &race_dbg;
    return !d->bench_frames && (d->sim ? ((d->drawn % 250 == 2 && !d->sim_race) || d->drawn == d->check_at)
                                       : (d->drawn == d->check_at && !pocket_race_logged++));
}

void race_debug_report(uint32_t st, volatile uint8_t *fb, int ask, uint32_t steps_now,
                       const struct track_stats *ts)
{
    struct race_debug *d = &race_dbg;
    if (!d->sim && d->drawn == d->check_at) dlog_str("track check: simulation only\n");
    frame_statistics(fb, ask, rdcycle() - d->t_check, ts);

    /* Pictures drawn in the last second (VID_HZ video frames). */
    if (((VID_FRAME(st) - d->fps_frame) & 0xFFFF) >= VID_HZ) {
        d->fps = d->drawn - d->fps_drawn;
        d->fps_drawn = d->drawn;
        d->fps_frame = VID_FRAME(st);
    }
    if (status_due()) {
        log_track_frame(ts, steps_now);
        log_cycles(ts);
        log_raster();
        tl_log();
    }
    d->frame_cycles = rdcycle() - d->t_frame;
    tl_mark(TL_END);
}

/* Shows the result of the benchmark (bench.h) on the screen, one line for each track, until a button is
   pressed. A new press counts only after 25 pictures. rows holds the summaries and ok[] tells which tracks
   were raced. */
static void bench_show(const struct ft_summary *rows, const int *ok)
{
    static const uint32_t pal[5] = { 0x000000, 0xFFFFFF, 0x40D070, 0xF04848, 0x909090 };
    enum { BG, FG, GREEN_, RED_, GREY_ };           /* the colours: black, white, green, red, grey */
    char line[CON_COLS + 1], *p;
    uint32_t drawn = 0, keys_before = IN_KEY;
    for (;;) {
        uint32_t st = wait_swap_done();
        volatile uint8_t *fb = FB(!(st & VID_FRONT));
        for (int i = 0; i < 5; i++) VID_PAL[i] = pal[i];
        fb_fill(fb, 0, 0, FB_W, FB_H, BG);
        fb_fill(fb, 0, 0, FB_W, 8, FG);
        con_text(fb, 1, 0, "SCREAMER - BENCHMARK", BG, FG);
        p = fmt_scale(fmt_str(line, "far "), track_far_scale());
        fmt_scale(fmt_str(p, "  lod "), track_lod_scale());
        con_text(fb, 1, 2, line, GREEN_, BG);
        con_text(fb, 1, 4, BENCH_SCREEN_HEAD, GREY_, BG);
        for (int t = 0; t < BENCH_TRACKS; t++) {
            bench_screen_row(line, t + 1, ok[t] ? &rows[t] : 0);
            con_text(fb, 1, 5 + t, line, ok[t] && rows[t].late ? RED_ : FG, BG);
        }
        con_text(fb, 1, 12, "avg, max: ms a frame; late: over 20 ms", GREY_, BG);
        con_text(fb, 1, 13, "faces, pieces: mean a frame", GREY_, BG);
        con_text(fb, 1, 15, "More in debug.txt (Saves folder).", GREY_, BG);
        con_text(fb, 1, 17, "Press a button to go on.", FG, BG);
        VID_SWAP = 1;
        uint32_t keys = IN_KEY;
        if (++drawn > 25 && (keys & ~keys_before)) return;
        keys_before = keys;
    }
}

/* The benchmark (bench.h), started by "bench = 1" in screamer.cfg. Each track is raced once for BENCH_FRAMES
   pictures with the gas held and the first car. The options (laps, difficulty) are as set in the menu. The
   result goes into debug.txt and onto the screen. The races write little to the log, and room for the result
   is kept free in it (dlog_keep). */
void race_debug_bench(int (*race)(int n, int car))
{
    if (!config_bench()) return;
    struct ft_summary rows[BENCH_TRACKS];
    int ok[BENCH_TRACKS] = { 0 };
    struct menu_options opt;
    menu_options(&opt);
    car_laps = opt.laps;
    car_mode = CAR_NORMAL;
    track_build_difficulty = opt.difficulty == 3 ? 4 : opt.difficulty;
    opp_bullet = track_build_difficulty > 2;
    speech_on = 0;
    dlog_keep(MARK_ROOM + BENCH_LOG_BYTES);
    for (int n = 1; n <= BENCH_TRACKS; n++) {
        race_dbg.bench_frames = BENCH_FRAMES;
        if (race(n, 0)) {
            ft_summary(&rows[n - 1]);
            ok[n - 1] = rows[n - 1].frames > 0;
        }
        race_dbg.bench_frames = 0;
    }
    dlog_keep(0);                               /* the result may use the room kept for it and the marks */
    bench_log(rows, ok, track_far_scale(), track_lod_scale());
    dlog_keep(MARK_ROOM);                       /* afterwards only the marks' room is kept */
    bench_show(rows, ok);
}
