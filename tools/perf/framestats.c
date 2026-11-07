/*
 * Frame statistics along a track's road (docs/development.md, "Frame statistics"). The firmware's own
 * track.c, geom.c, setup.c and raster.c (copies with counters, tools/perf/perf_patch.py) draw frames
 * from the near chase camera over road points; every frame's counters go into a CSV line, once per
 * number of cars. Built and run by tools/perf/framestats.sh. Not part of the firmware.
 *
 * Usage: framestats <track image> <label> <out.csv> <step> <cars,...> [grid]
 *   step   every step-th road point (MMP array 2) is a frame; -N: about N frames
 *   cars   comma list of car counts: 0 no car at all, 1 the player's car (always in the chase
 *          view), n the player's and n - 1 opponents ahead (see frame()); models AUTO00..06 in turn
 *   grid   (optional word) one more frame: the start grid, player at the start place
 * In the environment: FARSCALE=f: the far plane, reach and cell window times f, LODSCALE=f: the level-of-detail
 * distances times f (0: no level of detail); default what track.h has (FAR_SCALE, LOD_SCALE).
 * POPIN=d in the environment: each frame is drawn a second time with the level-of-detail distances d times larger (the
 * blocks the car passes in that step change level) and the pixels that differ are counted in a last column, popin_px
 * (the scene on a black screen; the cars keep the level they were made at).
 * The camera is the game's near chase camera (car.c, views[2]): 1300 behind, 300 above, looking
 * down 0.1885 rad, over a car standing on the road at that point.
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
#include "perf.h"

void perf_set_scales(float far_scale, float lod_scale);   /* track.c's copy (perf_patch.py) */

#define TEX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000))      /* as main.c */
#define MAX_CARS    10

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

static const struct track_dir *dirs;
static int nd;
static float popin_d, lod_scale = LOD_SCALE;        /* POPIN, and the level-of-detail factor in force (LODSCALE) */

/* the road a distance ahead of road point i: place and heading (the points run the way the road
   is driven when the next one lies in the heading's direction, else the other way) */
static void road_ahead(int i, float dist, float *x, float *y, float *yaw)
{
    const struct track_dir *q = &dirs[i % nd], *n = &dirs[(i + 1) % nd];
    float fx = sinf(q->angle), fy = -cosf(q->angle);
    int dir = ((float)(n->x - q->x) * fx + (float)(n->y - q->y) * fy) >= 0.0f ? 1 : -1;
    if (dist < 0.0f) { dir = -dir; dist = -dist; }      /* behind */
    float acc = 0.0f;
    int k = i;
    for (int guard = 0; guard < nd; guard++) {
        const struct track_dir *a = &dirs[(k % nd + nd) % nd], *b = &dirs[((k + dir) % nd + nd) % nd];
        float seg = hypotf((float)(b->x - a->x), (float)(b->y - a->y));
        if (acc + seg >= dist) {
            float t = seg > 0.0f ? (dist - acc) / seg : 0.0f;
            *x = (float)a->x + t * (float)(b->x - a->x);
            *y = (float)a->y + t * (float)(b->y - a->y);
            *yaw = a->angle;
            return;
        }
        acc += seg;
        k += dir;
    }
    *x = (float)q->x;
    *y = (float)q->y;
    *yaw = q->angle;
}

static float ground(float x, float y, float fallback)
{
    float h = fallback;
    int grass;
    return track_under(x, y, &h, &grass) ? h : fallback;
}

/* the near chase camera over a car standing at (x, y, z) heading yaw */
static struct track_cam chase_cam(float x, float y, float z, float yaw)
{
    const float behind = 1300.0f, above = 300.0f, down = 0.1885f;
    float pitch = -down;
    struct track_cam c;
    c.x = x - cosf(pitch) * behind * sinf(yaw);
    c.y = y + cosf(pitch) * behind * cosf(yaw);
    c.h = z + above - behind * sinf(pitch);
    c.yaw = yaw;
    c.pitch = pitch;
    c.roll = 0.0f;
    return c;
}

static void add_car(struct track_object *objs, int *nobj, struct track_block (*blocks)[CAR_BLOCKS], int model,
                    float x, float y, float z, float yaw, const struct track_cam *cam)
{
    int nm;
    track_models(&nm);
    struct car_look lk = { x, y, z, yaw, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0x4000u >> 2 };
    struct track_object o;
    memset(&o, 0, sizeof o);
    if (car_draw(model % (nm ? nm : 1), &lk, cam, blocks[*nobj], &o)) objs[(*nobj)++] = o;
}

/* one frame: the player's car at (x, y, z, yaw) if ncars >= 1, opponents ahead (or the grid's) */
static void frame(FILE *out, const char *label, int idx, float x, float y, float z, float yaw, int ncars, int grid,
                  const char *tag)
{
    static struct track_object objs[MAX_CARS + 2];
    static struct track_block blocks[MAX_CARS + 2][CAR_BLOCKS];
    int nobj = 0, nm;
    perf_reset();
    track_models(&nm);
    struct track_cam cam = chase_cam(x, y, z, yaw);
    if (ncars >= 1) add_car(objs, &nobj, blocks, 0, x, y, z, yaw, &cam);
    /* the start grid (track 1, measured in the game): nine opponents in rows 4,000 apart, two columns
       3,600 apart, the player last in the right column; (ahead, left) in units. The first is beside the
       player. On the road: opponents 2,200 ahead and then every 1,700, alternately 500 left and right of
       the middle. */
    static const float gridpos[9][2] = { { 0, 3398 }, { 4117, -200 }, { 4143, 3597 }, { 8217, -199 }, { 8186, 3687 },
                                         { 12378, -89 }, { 12357, 3694 }, { 15751, -131 }, { 15720, 3655 } };
    for (int k = 1; k < ncars && k < MAX_CARS; k++) {
        float ahead = grid ? gridpos[k - 1][0] : 2200.0f + 1700.0f * (float)(k - 1);
        float side = grid ? gridpos[k - 1][1] : (k & 1) ? 500.0f : -500.0f, ox, oy, oyaw;
        if (grid) {
            ox = x + sinf(yaw) * ahead + cosf(yaw) * side;
            oy = y - cosf(yaw) * ahead + sinf(yaw) * side;
            oyaw = yaw;
        } else {
            float rx, ry;
            road_ahead(idx, ahead, &rx, &ry, &oyaw);
            ox = rx + cosf(oyaw) * side;
            oy = ry + sinf(oyaw) * side;
        }
        add_car(objs, &nobj, blocks, k % (nm ? nm : 1), ox, oy, ground(ox, oy, z), oyaw, &cam);
    }
    const struct rs_clip clip = { 0, 0, FB_W, FB_H };
    struct track_stats ts;
    memset(&ts, 0, sizeof ts);
    uint32_t c1 = perf_c.lod_c1, c2 = perf_c.lod_c2, cd = perf_c.lod_cd;      /* counted when the cars were made */
    perf_reset();
    perf_c.lod_c1 = c1; perf_c.lod_c2 = c2; perf_c.lod_cd = cd;
    track_mode = TRACK_MODEL;
    static uint8_t keep[FB_W * FB_H];
    memset((void *)FB(0), 0, FB_W * FB_H);
    track_draw_cam(FB(0), &clip, &cam, objs, nobj, &ts);
    fprintf(out, "%s,%s,%d,%.0f,%.0f,%.0f,%d,%u,%u,%u,%u,%u", label, tag, idx, x, y, z, ncars, ts.pieces, ts.faces,
            ts.triangles, ts.vertices, ts.objects);
    perf_row(out);
    if (popin_d > 0.0f) {
        memcpy(keep, (const void *)FB(0), sizeof keep);
        perf_set_scales(0.0f, lod_scale * (1.0f + popin_d));
        memset((void *)FB(0), 0, FB_W * FB_H);
        track_draw_cam(FB(0), &clip, &cam, objs, nobj, &ts);
        perf_set_scales(0.0f, lod_scale);
        int diff = 0;
        for (int i = 0; i < FB_W * FB_H; i++) diff += keep[i] != ((const volatile uint8_t *)FB(0))[i];
        fprintf(out, ",%d", diff);
        if (getenv("POPIN_DUMP") && diff >= atoi(getenv("POPIN_DUMP"))) {    /* the two pictures of a frame that changes by that many pixels or more */
            char name[64];
            snprintf(name, sizeof name, "popin_%s_%d_a.raw", label, idx);
            FILE *fa = fopen(name, "wb");
            fwrite(keep, 1, sizeof keep, fa);
            fclose(fa);
            snprintf(name, sizeof name, "popin_%s_%d_b.raw", label, idx);
            fa = fopen(name, "wb");
            fwrite((const void *)FB(0), 1, sizeof keep, fa);
            fclose(fa);
        }
    }
    fprintf(out, "\n");
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: framestats <image> <label> <out.csv> <step> <cars,...> [grid]\n");
        return 2;
    }
    map();
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n = fread(TEX_FILE, 1, 0x700000, f);
    fclose(f);
    if (!track_open(TEX_FILE)) { fprintf(stderr, "not a track image (%zu bytes)\n", n); return 1; }
    int placements = track_init();
    if (!placements) { fprintf(stderr, "track_init: the image does not fit the limits\n"); return 1; }
    dirs = track_dirs(&nd);
    if (getenv("FARSCALE") || getenv("LODSCALE"))
        perf_set_scales(getenv("FARSCALE") ? (float)atof(getenv("FARSCALE")) : 0.0f, getenv("LODSCALE") ? (float)atof(getenv("LODSCALE")) : -1.0f);
    if (getenv("LODSCALE")) lod_scale = (float)atof(getenv("LODSCALE"));
    if (getenv("POPIN")) popin_d = (float)atof(getenv("POPIN"));
    int step = atoi(argv[4]);
    if (step < 0) step = nd / -step;            /* -N: about N frames */
    if (step < 1) step = 1;
    int cars[8], ncv = 0;
    for (char *s = strtok(argv[5], ","); s && ncv < 8; s = strtok(0, ",")) cars[ncv++] = atoi(s);

    FILE *out = fopen(argv[3], "w");
    if (!out) { perror(argv[3]); return 1; }
    fprintf(out, "label,kind,idx,x,y,z,ncars,ts_pieces,ts_faces,ts_triangles,ts_vertices,ts_objects");
    perf_header(out);
    fprintf(out, popin_d > 0.0f ? ",popin_px\n" : "\n");
    fprintf(stderr, "%s: %d placements, %d road points\n", argv[2], placements, nd);

    /* the road's height at every point; where track_under finds nothing (the middle of the road is
       not always over a drivable piece), that of the nearest point that has one */
    float *zr = malloc(sizeof(float) * (size_t)nd);
    char *ok = malloc((size_t)nd);
    int bad = 0;
    for (int i = 0; i < nd; i++) {
        float h = 0.0f;
        int grass;
        ok[i] = (char)track_under((float)dirs[i].x, (float)dirs[i].y, &h, &grass);
        zr[i] = h;
        bad += !ok[i];
    }
    for (int i = 0; i < nd; i++)
        for (int d = 1; !ok[i] && d < nd; d++) {
            int a = (i + d) % nd, b = (i - d + nd) % nd;
            if (ok[a]) { zr[i] = zr[a]; break; }
            if (ok[b]) { zr[i] = zr[b]; break; }
        }
    fprintf(stderr, "%d of %d road points have no drivable piece under them (height of the nearest one)\n", bad, nd);

    for (int i = 0; i < nd; i += step) {
        const struct track_dir *q = &dirs[i];
        for (int v = 0; v < ncv; v++) frame(out, argv[2], i, (float)q->x, (float)q->y, zr[i], q->angle, cars[v], 0, "road");
    }
    if (argc > 6 && !strcmp(argv[6], "grid")) {
        float x, y, yaw;
        track_start(&x, &y, &yaw);
        int near = 0;
        float bd = 1e30f;
        for (int i = 0; i < nd; i++) {
            float d = hypotf((float)dirs[i].x - x, (float)dirs[i].y - y);
            if (d < bd) { bd = d; near = i; }
        }
        float z = ground(x, y, zr[near]);
        fprintf(stderr, "start %.0f %.0f %.0f heading %.3f\n", x, y, z, yaw);
        for (int v = 0; v < ncv; v++) frame(out, argv[2], -1, x, y, z, yaw, cars[v], 1, "grid");
    }
    fclose(out);
    return 0;
}
