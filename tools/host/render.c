/*
 * render.c - one frame of the race from the firmware's own code on the PC, through the C models
 * of the geometry unit, the setup unit and the rasterizer (the firmware's TRACK_MODEL path, the
 * one its track check sets against the hardware). For comparing with the original
 * (tools/cmpframe.py) in seconds instead of a simulation's minutes. Built by tools/render.sh.
 *
 * The firmware's addresses are kept: the SDRAM is mapped at SDRAM_BASE (and its uncached view
 * at SDRAM_UBASE, the same memory), the registers and frame buffers at 0xF0000000, so its
 * pointers and SDRAM byte addresses work unchanged.
 *
 * Usage: render <track image> <MAPnH.DAT> <MAPn.DAT> <out.rgb> [view [steps [n [car]]]]
 *   view: 0 bumper, 1 cockpit, 2 near chase (default), 3 far chase; steps: physics steps on
 *   the grid before the picture (default 2); n: the track's number (default 1); car: the
 *   player's row of CARS.DTR (default 0), the opponents take the others
 *   GAS=1 in the environment: the pedal down for the steps; TURN=k: the last k steering left (-k
 *   right), BRAKE=k: the last k braking. PAUSE=1: the pause's picture over the frame (the race screen left out),
 *   with the disc's tracks 2-8, 3 playing (GAME.DAT for the CD player in the working directory);
 *   PAUSE=2: PLAY AGAIN? TRACKSIDE=1: the trackside camera after the steps, HUD=1 with the race
 *   screen. REPLAYCHECK=s: the replay's determinism check instead (replay_check()), REPLAYAT=k
 *   its picture at step k of the replay, NOHUD=1 without the race screen, REPLAYDUMP=f: the
 *   race's values of every step and each drawn frame's CRC-32 and race screen into file f (two
 *   versions of the code compared with cmp). MODE=m: the game
 *   mode (car.h, 4 time attack, 0x20 cone carnage, 0x40 slalom), its props, rules and race screen;
 *   GHOST=1 with MODE=4: ghost_check() instead of replay_check().
 *        render menu <MENU.DAT> <GRAF.DAT> <LOCAL.DAT> <GEN.DAT> <out.rgb> [screen [n [steps]]]
 *   the track screen (screen 1, the default) with track n chosen or the car screen (screen 2)
 *   with the car of row n - 1 (menu.h; default 1), steps of 1/50 s after entering it (default 0);
 *   screen 3: track n's loading screen; screen 4 the main menu, 5 the options page; screen 6 the
 *   championship's stage CHAMP="s[,failed]" (default 0), races before it with made-up results;
 *   screen 7 VIEW RECORDS of track n, two made-up races entered (choice_record); screen 8 ENTER
 *   YOUR NAME.
 *   MENUKEYS in the environment: presses before the steps, each for one step, then 30 steps
 *   without (U D L R up, down, left, right; B; A)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include "hw.h"
#include "track.h"
#include "car.h"
#include "opp.h"
#include "ghost.h"
#include "efx.h"
#include "scenery.h"
#include "blit.h"
#include "menu.h"
#include "champ.h"
#include "hud.h"
#include "crc32.h"
#include "intro.h"

#define MENU_FILE   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x200000))       /* as main.c */
#define GRAF_FILE   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x600000))
#define MENU_MEM    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0xA00000))
#define GEN_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2D40000))
#define LOCAL_FILE  ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2D50000))
#define SCRATCH     ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2D60000))
#define TEX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000))      /* as main.c */
#define GFX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2800000))
#define EFX_SIZE    0x10000
#define EFX_MEM     ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE))
#define SCENERY_MEM   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - 0x20000))
#define DATA_FILE   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2D00000))
#define GAME_FILE   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2C00000))
#define STILL_MEM   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x110000))       /* as main.c */
#define CD_MEM      ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x120000))
#define MAX_OBJECTS 64

static void map(void)
{
    int fd = memfd_create("sdram", 0);
    if (fd < 0 || ftruncate(fd, SDRAM_SIZE)
        || mmap((void *)(uintptr_t)SDRAM_BASE, SDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
        || mmap((void *)(uintptr_t)SDRAM_UBASE, SDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
        || mmap((void *)0xF0000000u, 0x4000000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        perror("map");
        exit(1);
    }
}

static uint32_t load(const char *path, uint8_t *at, uint32_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint32_t n = (uint32_t)fread(at, 1, max, f);
    fclose(f);
    return n;
}

static void write_rgb(const char *path, const uint32_t *pal)
{
    volatile uint8_t *fb = FB(0);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    for (int i = 0; i < FB_W * FB_H; i++) {
        uint32_t c = pal[fb[i]];
        uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* The replay's check (REPLAYCHECK=steps): a race driven by a robot at the keys (gas held, the
   d-pad towards the path 20 points ahead, the brake in sharp turns, R tapped now and then),
   recorded as main.c records it (car_keys), in frames of 1-4 steps with the objects and the
   race screen made between them as main.c makes them, every 32nd frame drawn; it stops 250
   steps after the finish, or after steps. Then everything set up again as main.c's race() does
   and the race run from the recording on the trackside camera, in other frames; the player's
   place, heading and speed and the opponents' places compared bit for bit after every step. */
#define CHECK_MAX   60000
#define CHECK_VALS  (5 + 2 * 16)
static struct car_input rec[CHECK_MAX];
static float vals[CHECK_MAX][CHECK_VALS];
static FILE *dump;                      /* REPLAYDUMP: the race's steps and frames */

static uint32_t robot(uint32_t step)
{
    static uint32_t at;
    const struct car_state *c = car_state();
    uint32_t np = track_path_length(), best_i = at;
    float best = 1e30f, x, y, h;
    for (int d = -20; d <= 60; d++) {
        uint32_t i = (at + np + (uint32_t)(d + (int)np)) % np;
        track_path_point(i, &x, &y, &h);
        float dd = (x - c->x) * (x - c->x) + (y - c->y) * (y - c->y);
        if (dd < best) { best = dd; best_i = i; }
    }
    at = best_i;
    track_path_point((at + 20) % np, &x, &y, &h);
    float err = atan2f(x - c->x, -(y - c->y)) - c->yaw;
    while (err > 3.14159265f) err -= 6.2831853f;
    while (err < -3.14159265f) err += 6.2831853f;
    uint32_t k = KEY_B;
    if (err > 0.03f) k |= KEY_LEFT;
    if (err < -0.03f) k |= KEY_RIGHT;
    if (fabsf(err) > 0.8f && c->speed > 6000.0f) k = (k & ~KEY_B) | KEY_Y;
    if (step % 700 == 350) k |= KEY_R;
    return k;
}

static void check_values(float *v)
{
    const struct car_state *c = car_state();
    v[0] = c->x; v[1] = c->y; v[2] = c->z; v[3] = c->yaw; v[4] = c->speed;
    for (int k = 0; k < 16; k++) {
        float x = 0.0f, y = 0.0f;
        if (k < opp_count()) opp_where(k, &x, &y, 0);
        v[5 + 2 * k] = x;
        v[6 + 2 * k] = y;
    }
}

static void write_rgb(const char *path, const uint32_t *pal);

/* the objects and the race screen between frames, as main.c makes them; every 32nd frame drawn,
   and with out the whole picture (backdrop, scene, race screen unless NOHUD=1) into that file,
   its camera and the car printed */
static void check_frame(uint32_t frame, uint32_t steps_now, const char *out)
{
    static struct track_object objs[MAX_OBJECTS];
    static struct blit_list l;
    struct track_cam cam;
    struct track_stats ts;
    track_clock(steps_now);
    car_camera(&cam);
    int nobj = car_object(&cam, &objs[0]);
    nobj += opp_objects(objs + nobj, MAX_OBJECTS - nobj);
    nobj += ghost_objects(objs + nobj, MAX_OBJECTS - nobj);
    nobj += car_effects(&cam, objs + nobj, MAX_OBJECTS - nobj);
    nobj += scenery_objects(&cam, objs + nobj, MAX_OBJECTS - nobj);
    if (frame % 32 == 0 || out) {
        const struct rs_clip clip = { 0, 0, FB_W, FB_H };
        blit_start(&l, 0);
        track_backdrop_blit(&l, &clip, &cam);
        blit_model(FB(0), &l);
        track_mode = TRACK_MODEL;
        track_draw_cam(FB(0), &clip, &cam, objs, nobj, &ts);
    }
    hud_build(&l, 0, 0, car_overlay());
    if (dump && frame % 32 == 0) {
        uint32_t crc = crc32((const uint8_t *)FB(0), (const uint8_t *)FB(0) + FB_W * FB_H);
        fwrite(&crc, 4, 1, dump);
        fwrite(&l.n, 4, 1, dump);
        fwrite(l.c, 16, (size_t)l.n, dump);
    }
    if (out) {
        printf("picture: camera %.0f %.0f %.0f yaw %.4f pitch %.4f, car %.0f %.0f %.0f\n", cam.x, cam.y, cam.h, cam.yaw,
               cam.pitch, car_state()->x, car_state()->y, car_state()->z);
        if (!getenv("NOHUD")) blit_model(FB(0), &l);
        write_rgb(out, track_palette());
    }
}

/* GHOST=1 with MODE=4: time attack's ghost (ghost.h) instead. A robot's race as replay_check's,
   kept as the track's best if it finishes; then a second one against it from the other grid
   place, its picture at step REPLAYAT (default: its end) with the ghost and the race screen. At
   each step 4k + 1 (a pose exactly) of every 500 the ghost's place is set against the first
   race's player's, and the running order printed. */
static int ghost_check(int max, int car, int n, uint32_t data_size, uint32_t gfx_size, const char *out)
{
    static float first[CHECK_MAX][3];
    int props, moving;
    uint32_t shot = getenv("REPLAYAT") ? (uint32_t)atoi(getenv("REPLAYAT")) : ~0u;
    if (max > CHECK_MAX - 1) max = CHECK_MAX - 1;
    for (int run = 0; run < 2; run++) {
        uint32_t steps = 0, frames = 0, seed = 1, keys_before = 0, end = 0;
        ghost_resume = run;
        scenery_init(DATA_FILE, data_size, GFX_FILE, gfx_size, n, SCENERY_MEM, 0x10000, &props, &moving);
        ghost_init(n, car);
        car_init(car, 0);
        car_view(CAR_VIEW_START);
        opp_init(car);
        printf("run %d: grid place %d, start %.0f %.0f, ghost's lap %.2f s\n", run, ghost_slot(), car_state()->x,
               car_state()->y, ghost_time());
        while ((int)steps < max && (!end || steps < end)) {
            seed = seed * 1103515245u + 12345u;
            uint32_t now = 1 + (seed >> 16) % 4;
            for (uint32_t k = 0; k < now && (int)steps < max; k++) {
                uint32_t keys = robot(steps);
                struct car_input in = car_keys(keys, keys_before, 0, 0);
                car_step(&in);
                keys_before = keys;
                opp_step();
                scenery_step();
                steps++;
                const struct car_state *c = car_state();
                struct track_object g;
                if (!run) {
                    first[steps][0] = c->x; first[steps][1] = c->y; first[steps][2] = c->z;
                } else if (steps % 500 == 1 && ghost_objects(&g, 1))
                    printf("  step %u: ghost %.0f %.0f %.0f, first race %.0f %.0f %.0f, player %.0f %.0f, place %d\n",
                           steps, g.x, g.y, g.h, first[steps][0], first[steps][1], first[steps][2], c->x, c->y,
                           opp_player_rank());
                if (car_race()->finished && !end) end = steps + 250;
            }
            check_frame(frames++, now, run && steps >= shot ? out : 0);
            if (run && steps >= shot) shot = ~0u;
        }
        const struct car_race *r = car_race();
        printf("run %d: %u steps, finished %d, time over %d, lap %.2f s, made it %d, place %d, lap to beat %.2f\n", run,
               steps, r->finished, r->game_over, r->total, r->made_it, opp_player_rank(), car_lap_record());
        if (run && shot != ~0u) check_frame(frames, 0, out);
        ghost_end();
        const struct ghost_file *f = ghost_track(n);
        printf("  kept: %.4s, car %d, %d poses, %u steps (%u bytes)\n", f->magic, f->car, f->n, f->steps,
               16 + 20 * f->n);
    }
    return 0;
}

static int replay_check(int max, int car, int n, uint32_t data_size, uint32_t gfx_size, const char *out)
{
    int props, moving;
    uint32_t steps = 0, frames = 0, seed = 1, keys_before = 0, end = 0;
    if (max > CHECK_MAX) max = CHECK_MAX;
    if (getenv("REPLAYDUMP")) dump = fopen(getenv("REPLAYDUMP"), "wb");
    car_init(car, 0);
    opp_init(car);
    while ((int)steps < max && (!end || steps < end)) {
        seed = seed * 1103515245u + 12345u;
        uint32_t now = 1 + (seed >> 16) % 4;
        for (uint32_t k = 0; k < now && (int)steps < max; k++) {
            uint32_t keys = robot(steps);
            rec[steps] = car_keys(keys, keys_before, 0, 0);
            keys_before = keys;
            car_step(&rec[steps]);
            opp_step();
            scenery_step();
            check_values(vals[steps++]);
            if (dump) fwrite(vals[steps - 1], sizeof vals[0], 1, dump);
            if (car_race()->finished && !end) end = steps + 250;
        }
        check_frame(frames++, now, 0);
    }
    if (dump) {
        fclose(dump);
        dump = 0;
    }
    const struct car_race *r = car_race();
    printf("race: %u steps in %u frames, %d laps (progress %.1f), finished %d, time over %d, place %d, walls %u, "
           "crashes %u\n",
           steps, frames, r->laps, car_progress(), r->finished, r->game_over, opp_player_rank(), car_wall_hits(),
           car_crashes());
    int all, down = scenery_hits(&all), gates, through = car_gates(&gates);
    printf("mode 0x%x: props down %d of %d, gates %d of %d, countdown %.2f, perfect %d\n", car_mode, down, all,
           through, gates, r->timer, r->perfect);
    /* again, as race() sets it up: the scenery, then the car on the trackside camera */
    scenery_init(DATA_FILE, data_size, GFX_FILE, gfx_size, n, SCENERY_MEM, 0x10000, &props, &moving);
    car_init(car, 1);
    opp_init(car);
    uint32_t at = 0, replay_frames = 0, shot = getenv("REPLAYAT") ? (uint32_t)atoi(getenv("REPLAYAT")) : steps;
    seed = 2;
    while (at < steps) {
        seed = seed * 1103515245u + 12345u;
        uint32_t now = 1 + (seed >> 16) % 4;
        for (uint32_t k = 0; k < now && at < steps; k++) {
            car_step(&rec[at]);
            opp_step();
            scenery_step();
            float v[CHECK_VALS];
            check_values(v);
            if (memcmp(v, vals[at], sizeof v)) {
                int j = 0;
                while (v[j] == vals[at][j]) j++;
                printf("replay: step %u differs, value %d: %.6f, the race had %.6f\n", at, j, v[j], vals[at][j]);
                return 1;
            }
            at++;
        }
        check_frame(replay_frames++, now, at >= shot ? out : 0);
        if (at >= shot) shot = ~0u;
    }
    printf("replay: %u steps in %u frames, %d laps, finished %d: every step identical "
           "(the player and %d opponents)\n", at, replay_frames, car_race()->laps, car_race()->finished, opp_count());
    return 0;
}

/* a screen of the menu: the containers whole (the firmware loads only menu.h's parts of them) */
static int menu(int argc, char **argv)
{
    int which = argc > 7 ? atoi(argv[7]) : 1;
    int screen = which == 2 ? MENU_CAR_SCREEN : which == 6 ? MENU_CHAMP_SCREEN : which == 7 ? MENU_RECORDS_SCREEN
               : which == 8 ? MENU_NAME_SCREEN : which >= 4 ? MENU_PAGE_SCREEN : MENU_TRACK_SCREEN;
    int n = which == 4 || which == 5 ? which - 3 : argc > 8 ? atoi(argv[8]) : 1, steps = argc > 9 ? atoi(argv[9]) : 0;
    uint32_t menu_size = load(argv[2], MENU_FILE, 0x400000), graf_size = load(argv[3], GRAF_FILE, 0x400000);
    uint32_t local_size = load(argv[4], LOCAL_FILE, 0x10000), gen_size = load(argv[5], GEN_FILE, 0x10000);
    if (!menu_init(MENU_FILE, menu_size, GRAF_FILE, graf_size, LOCAL_FILE, local_size, GEN_FILE, gen_size, MENU_MEM,
                   0x80000, SCRATCH, 0x2A0000)) { fprintf(stderr, "menu: its files are not all there\n"); return 1; }
    static struct blit_list l;
    volatile uint8_t *fb = FB(0);
    if (which == 3) {
        blit_start(&l, 0);
        const uint32_t *pal = menu_loading(n, &l);
        if (!pal) { fprintf(stderr, "menu: no loading screen for track %d\n", n); return 1; }
        blit_model(fb, &l);
        write_rgb(argv[6], pal);
        return 0;
    }
    choice_defaults();
    if (which == 6) {                   /* the stage, the races before it 1st, 2nd, 3rd in turn */
        static const int pts[3] = { 9, 7, 5 };
        const char *c = getenv("CHAMP");
        int failed = 0;
        if (c) sscanf(c, "%d,%d", &choice.stage, &failed);
        struct champ_stage st = champ_at(choice.stage);
        for (int i = 0; i < 6 && st.kind == CHAMP_STANDINGS && i <= st.race; i++) {
            choice.pos[i] = i % 3 + 1;
            choice.time[i] = 151.37f + 9.5f * (float)i;
            choice.pts[i] = pts[i % 3];
        }
        choice.failed = failed;
    }
    if (which == 7) {                   /* a three-lap race by DEN, then a longer one by ABC */
        memcpy(choice.name, "DEN", 4);
        choice_record(n, 75.37f, 241.52f, 1, CAR_NORMAL);
        memcpy(choice.name, "ABC", 4);
        choice_record(n, 77.01f, 420.0f, 0, CAR_NORMAL);
    }
    menu_enter(screen, n);
    for (const char *s = getenv("MENUKEYS"); s && *s; s++) {
        static const char name[] = "UDLRBA";
        static const uint32_t key[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_B, KEY_A };
        const char *at = strchr(name, *s);
        if (at) menu_step(key[at - name], 0);
        for (int k = 0; k < 30; k++) menu_step(0, 0);
    }
    for (int k = 0; k < steps; k++) menu_step(0, 0);
    blit_start(&l, 0);
    menu_backdrop(&l);
    blit_model(fb, &l);
    track_mode = TRACK_MODEL;
    menu_scene(fb);
    blit_start(&l, 0);
    menu_text(&l);
    blit_model(fb, &l);
    write_rgb(argv[6], menu_palette());
    return 0;
}

/* INTRO=1: the intro's picture after steps steps of its race (main.c's run_intro), the logos
   and credits over it (GRAF.DAT and LOCAL.DAT in the working directory) */
static int intro_frame(int steps, int car, int n, uint32_t data_size, const char *out)
{
    uint32_t graf_size = load("GRAF.DAT", GRAF_FILE, 0x400000), local_size = load("LOCAL.DAT", LOCAL_FILE, 0x10000);
    if (!intro_init(GRAF_FILE, graf_size, LOCAL_FILE, local_size, MENU_MEM, 0x40000, SCRATCH, 0x2A0000)) {
        fprintf(stderr, "intro: its files are not all there\n");
        return 1;
    }
    car_mode = CAR_INTRO;
    car_init(car, 0);
    opp_init(car);
    car_intro();
    intro_start(DATA_FILE, data_size, n);
    for (int k = 0; k < steps; k++) {
        opp_step();
        scenery_step();
        intro_step();
    }
    track_clock((uint32_t)steps);
    struct track_cam cam;
    static struct track_object objs[MAX_OBJECTS];
    intro_camera(&cam);
    int nobj = opp_objects(objs, MAX_OBJECTS);
    nobj += scenery_objects(&cam, objs + nobj, MAX_OBJECTS - nobj);
    const struct rs_clip clip = { 0, 0, FB_W, FB_H };
    volatile uint8_t *fb = FB(0);
    static struct blit_list l;
    blit_start(&l, 0);
    track_backdrop_blit(&l, &clip, &cam);
    blit_model(fb, &l);
    struct track_stats ts;
    track_mode = TRACK_MODEL;
    track_draw_cam(fb, &clip, &cam, objs, nobj, &ts);
    blit_start(&l, 0);
    intro_text(&l);
    blit_model(fb, &l);
    write_rgb(out, track_palette());
    printf("camera %.0f %.0f %.0f yaw %.4f pitch %.4f; %d pieces, %d faces, %d objects\n",
           cam.x, cam.y, cam.h, cam.yaw, cam.pitch, ts.pieces, ts.faces, nobj);
    return 0;
}

int main(int argc, char **argv)
{
    map();
    if (argc >= 7 && !strcmp(argv[1], "menu")) return menu(argc, argv);
    if (argc < 5) { fprintf(stderr, "usage: render <track image> <MAPnH.DAT> <MAPn.DAT> <out.rgb> [view [steps [n [car]]]]\n"); return 2; }
    int view = argc > 5 ? atoi(argv[5]) : CAR_VIEW_START, steps = argc > 6 ? atoi(argv[6]) : 2;
    int n = argc > 7 ? atoi(argv[7]) : 1, car = argc > 8 ? atoi(argv[8]) : 0;
    MEM_STATUS = MEM_LOCKED | MEM_READY | MEM_WRITES_IDLE;     /* waited for by efx_init */
    load(argv[1], TEX_FILE, 0x700000);
    if (!track_open(TEX_FILE) || !track_init()) { fprintf(stderr, "track image not usable\n"); return 1; }
    uint32_t gfx_size = load(argv[2], GFX_FILE, 0x400000);
    efx_init(GFX_FILE, gfx_size, n, track_mlb(), track_palette(), EFX_MEM, EFX_SIZE);
    uint32_t data_size = load(argv[3], DATA_FILE, 0x40000);
    int props, moving;
    if (getenv("MODE")) car_mode = (int)strtol(getenv("MODE"), 0, 0);    /* MODE=0x20: cone carnage */
    scenery_init(DATA_FILE, data_size, GFX_FILE, gfx_size, n, SCENERY_MEM, 0x10000, &props, &moving);
    printf("scenery: %d props, %d moving\n", props, moving);
    car_trackside_init(DATA_FILE, data_size, n);
    car_mode_init(DATA_FILE, data_size, n, 1);         /* NORMAL, as make_track.py */
    if (getenv("REPLAYCHECK") && getenv("GHOST"))
        return ghost_check(atoi(getenv("REPLAYCHECK")), car, n, data_size, gfx_size, argv[4]);
    if (getenv("REPLAYCHECK")) return replay_check(atoi(getenv("REPLAYCHECK")), car, n, data_size, gfx_size, argv[4]);
    if (getenv("INTRO")) return intro_frame(steps, car, n, data_size, argv[4]);
    car_init(car, 0);
    opp_init(car);
    car_view(view);
    int turn = getenv("TURN") ? atoi(getenv("TURN")) : 0, brake = getenv("BRAKE") ? atoi(getenv("BRAKE")) : 0;
    for (int k = 0; k < steps; k++) {
        uint32_t keys = getenv("GAS") ? KEY_B : 0;  /* GAS=1: the pedal down */
        if (k >= steps - abs(turn)) keys |= turn > 0 ? KEY_LEFT : KEY_RIGHT;   /* TURN=k: the last k steps
                                                                                  left, -k right */
        if (k >= steps - brake) keys = (keys & ~KEY_B) | KEY_Y;                /* BRAKE=k: braking */
        struct car_input in = { (uint16_t)keys, 0x80, 0, 0 };    /* (no stick: car_keys) */
        car_step(&in);
        opp_step();
        scenery_step();
    }
    track_clock((uint32_t)steps);
    if (getenv("TRACKSIDE")) car_trackside();       /* TRACKSIDE=1: the race's end's camera */

    struct track_cam cam;
    static struct track_object objs[MAX_OBJECTS];
    car_camera(&cam);
    const char *set = getenv("CAM");    /* "x y h yaw pitch roll": the original's camera record */
    if (set) sscanf(set, "%f %f %f %f %f %f", &cam.x, &cam.y, &cam.h, &cam.yaw, &cam.pitch, &cam.roll);
    const char *rf = getenv("REFL");    /* "x y z [phase]": the player's reflection accumulators and
                                           picture (record +0x30, +0x3c) */
    float acc[3];
    unsigned rph = 0;
    if (rf && sscanf(rf, "%f %f %f %u", &acc[0], &acc[1], &acc[2], &rph) >= 3) car_reflection(acc, rph);
    int nobj = car_object(&cam, &objs[0]);
    nobj += opp_objects(objs + nobj, MAX_OBJECTS - nobj);
    nobj += ghost_objects(objs + nobj, MAX_OBJECTS - nobj);
    nobj += car_effects(&cam, objs + nobj, MAX_OBJECTS - nobj);
    nobj += scenery_objects(&cam, objs + nobj, MAX_OBJECTS - nobj);
    const struct rs_clip clip = { 0, 0, FB_W, car_cockpit() ? car_view_lines() : FB_H };
    volatile uint8_t *fb = FB(0);
    static struct blit_list back;
    blit_start(&back, 0);
    track_backdrop_blit(&back, &clip, &cam);
    blit_model(fb, &back);
    struct track_stats ts;
    track_mode = TRACK_MODEL;
    track_draw_cam(fb, &clip, &cam, objs, nobj, &ts);
    if (getenv("HUD")) {                                /* HUD=1: the race screen over it */
        static struct blit_list l;
        hud_build(&l, 0, 0, car_overlay());
        blit_model(fb, &l);
    }
    const char *pause = getenv("PAUSE");
    if (pause && *pause != '0') {                       /* main.c's still(), pause_race(), play_again() */
        int ask = atoi(pause) == 2;
        const uint8_t *d = efx_darken();
        for (int i = 0; i < FB_W * FB_H; i++) {
            uint8_t c = fb[i];
            for (int k = 0; d && k < 2 + ask; k++) c = d[c];
            STILL_MEM[i] = c;
        }
        static struct blit_list l;
        if (ask) hud_question(&l, 0, STILL_MEM, 0, 1);
        else {
            if (!hud_cd_init(GAME_FILE, load("GAME.DAT", GAME_FILE, 0x100000), CD_MEM, 0x10000, SCRATCH, 0x2A0000))
                fprintf(stderr, "CDFRONTL.PKC missing\n");
            hud_cd(&l, 0, STILL_MEM, 2, 8, 3, 0);
        }
        blit_model(fb, &l);
    }

    write_rgb(argv[4], track_palette());
    for (int k = 0; k < opp_count(); k++) {
        float x, y;
        if (opp_where(k, &x, &y, 0)) printf("opponent %d at %.1f %.1f\n", k, x, y);
    }
    printf("camera %.0f %.0f %.0f yaw %.4f pitch %.4f; %d pieces, %d faces, %d objects\n",
           cam.x, cam.y, cam.h, cam.yaw, cam.pitch, ts.pieces, ts.faces, nobj);
    return 0;
}
