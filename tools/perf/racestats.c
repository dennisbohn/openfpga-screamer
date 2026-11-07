/*
 * Frame statistics along a race (docs/development.md, "Frame statistics"): a race on the PC with a robot
 * at the keys (the one of tools/host/render.c: gas held, the d-pad towards the path 20 points ahead, the
 * brake in sharp turns), the player and all the opponents driving, the scene as main.c makes it (the
 * game's chase camera, the cars where they are, shadows and smoke, props), drawn every `every` steps with
 * the counters of perf_patch.py; one CSV line per frame, as framestats.c has it plus the step and the
 * player's speed. Built and run by tools/perf/framestats.sh. Not part of the firmware.
 *
 * Usage: racestats <track image> <MAPnH.DAT> <MAPn.DAT> <label> <out.csv> <steps> <every> <n> [car]
 *   steps  the race's length in game steps (1/50 s); it ends sooner 250 steps after the finish
 *   every  a frame every `every` steps (the first 300 steps are the start lights: the grid, 10 cars)
 * FARSCALE, LODSCALE in the environment as framestats has them.
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
#include "hud.h"
#include "perf.h"

void perf_set_scales(float far_scale, float lod_scale);     /* track.c's copy (perf_patch.py) */

#define GFX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2800000))
#define TEX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000))
#define DATA_FILE   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2D00000))
#define EFX_SIZE    0x10000
#define EFX_MEM     ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE))
#define SCENERY_MEM ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - 0x20000))
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

/* the robot of tools/host/render.c */
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

int main(int argc, char **argv)
{
    if (argc < 10) {
        fprintf(stderr, "usage: racestats <image> <MAPnH.DAT> <MAPn.DAT> <label> <out.csv> <steps> <every> <n> [car]\n");
        return 2;
    }
    map();
    int steps_max = atoi(argv[6]), every = atoi(argv[7]), n = atoi(argv[8]), car = argc > 9 ? atoi(argv[9]) : 0;
    MEM_STATUS = MEM_LOCKED | MEM_READY | MEM_WRITES_IDLE;
    load(argv[1], TEX_FILE, 0x700000);
    if (!track_open(TEX_FILE) || !track_init()) { fprintf(stderr, "track image not usable\n"); return 1; }
    if (getenv("FARSCALE") || getenv("LODSCALE"))
        perf_set_scales(getenv("FARSCALE") ? (float)atof(getenv("FARSCALE")) : 0.0f, getenv("LODSCALE") ? (float)atof(getenv("LODSCALE")) : -1.0f);
    uint32_t gfx_size = load(argv[2], GFX_FILE, 0x400000);
    efx_init(GFX_FILE, gfx_size, n, track_mlb(), track_palette(), EFX_MEM, EFX_SIZE);
    uint32_t data_size = load(argv[3], DATA_FILE, 0x40000);
    int props, moving;
    scenery_init(DATA_FILE, data_size, GFX_FILE, gfx_size, n, SCENERY_MEM, 0x10000, &props, &moving);
    car_trackside_init(DATA_FILE, data_size, n);
    car_mode_init(DATA_FILE, data_size, n, 1);
    car_init(car, 0);
    opp_init(car);
    car_view(CAR_VIEW_START);

    FILE *out = fopen(argv[5], "w");
    if (!out) { perror(argv[5]); return 1; }
    fprintf(out, "label,kind,idx,x,y,z,ncars,ts_pieces,ts_faces,ts_triangles,ts_vertices,ts_objects");
    perf_header(out);
    fprintf(out, ",speed,place\n");

    static struct track_object objs[MAX_OBJECTS];
    uint32_t steps = 0, keys_before = 0, seed = 1, end = 0;
    while ((int)steps < steps_max && (!end || steps < end)) {
        seed = seed * 1103515245u + 12345u;
        uint32_t now = 1 + (seed >> 16) % 2;               /* one or two steps a frame */
        for (uint32_t k = 0; k < now; k++) {
            uint32_t keys = robot(steps);
            struct car_input in = car_keys(keys, keys_before, 0, 0);
            keys_before = keys;
            car_step(&in);
            opp_step();
            scenery_step();
            steps++;
            if (car_race()->finished && !end) end = steps + 250;
        }
        track_clock(now);
        if (steps % (uint32_t)every >= now) continue;       /* one frame every `every` steps */
        struct track_cam cam;
        struct track_stats ts;
        car_camera(&cam);
        perf_reset();
        int nobj = car_object(&cam, &objs[0]);
        nobj += opp_objects(objs + nobj, MAX_OBJECTS - nobj);
        nobj += ghost_objects(objs + nobj, MAX_OBJECTS - nobj);
        nobj += car_effects(&cam, objs + nobj, MAX_OBJECTS - nobj);
        nobj += scenery_objects(&cam, objs + nobj, MAX_OBJECTS - nobj);
        uint32_t c1 = perf_c.lod_c1, c2 = perf_c.lod_c2, cd = perf_c.lod_cd;      /* counted when the cars were made */
        perf_reset();
        perf_c.lod_c1 = c1; perf_c.lod_c2 = c2; perf_c.lod_cd = cd;
        const struct rs_clip clip = { 0, 0, FB_W, FB_H };
        memset(&ts, 0, sizeof ts);
        track_mode = TRACK_MODEL;
        track_draw_cam(FB(0), &clip, &cam, objs, nobj, &ts);
        fprintf(out, "%s,race,%u,%.0f,%.0f,%.0f,%d,%u,%u,%u,%u,%u", argv[4], steps, cam.x, cam.y, cam.h, opp_count() + 1, ts.pieces,
                ts.faces, ts.triangles, ts.vertices, ts.objects);
        perf_row(out);
        fprintf(out, ",%.0f,%d\n", car_state()->speed, opp_player_rank());
    }
    fclose(out);
    fprintf(stderr, "%s: %u steps, %d laps, finished %d\n", argv[4], steps, car_race()->laps, car_race()->finished);
    return 0;
}
