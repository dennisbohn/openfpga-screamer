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
 * Usage: render <track image> <MAPnH.DAT> <MAPn.DAT> <out.rgb> [view [steps]]
 *   view: 0 bumper, 1 cockpit, 2 near chase (default), 3 far chase; steps: physics steps on
 *   the grid before the picture (default 2)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "hw.h"
#include "track.h"
#include "car.h"
#include "opp.h"
#include "efx.h"
#include "props.h"
#include "blit.h"

#define TEX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000))      /* as main.c */
#define GFX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2800000))
#define EFX_SIZE    0x10000
#define EFX_MEM     ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE))
#define PROPS_MEM   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - 0x20000))
#define DATA_FILE   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2D00000))
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

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: render <track image> <MAPnH.DAT> <MAPn.DAT> <out.rgb> [view [steps]]\n"); return 2; }
    int view = argc > 5 ? atoi(argv[5]) : CAR_VIEW_START, steps = argc > 6 ? atoi(argv[6]) : 2;
    map();
    MEM_STATUS = MEM_LOCKED | MEM_READY | MEM_WRITES_IDLE;     /* waited for by efx_init */
    load(argv[1], TEX_FILE, 0x700000);
    if (!track_open(TEX_FILE) || !track_init()) { fprintf(stderr, "track image not usable\n"); return 1; }
    uint32_t gfx_size = load(argv[2], GFX_FILE, 0x400000);
    efx_init(GFX_FILE, gfx_size, 1, track_mlb(), track_palette(), EFX_MEM, EFX_SIZE);
    uint32_t data_size = load(argv[3], DATA_FILE, 0x40000);
    printf("props: %d\n", props_init(DATA_FILE, data_size, GFX_FILE, gfx_size, 1, PROPS_MEM, 0x10000));
    car_init(0);
    opp_init(0);
    car_view(view);
    for (int k = 0; k < steps; k++) {
        car_step(0, 0);
        opp_step();
    }
    track_clock((uint32_t)steps);

    struct track_cam cam;
    static struct track_object objs[MAX_OBJECTS];
    car_camera(&cam);
    const char *set = getenv("CAM");    /* "x y h yaw pitch roll": the original's camera record */
    if (set) sscanf(set, "%f %f %f %f %f %f", &cam.x, &cam.y, &cam.h, &cam.yaw, &cam.pitch, &cam.roll);
    int nobj = car_object(&cam, &objs[0]);
    nobj += opp_objects(objs + nobj, MAX_OBJECTS - nobj);
    nobj += car_effects(&cam, objs + nobj, MAX_OBJECTS - nobj);
    nobj += props_objects(&cam, objs + nobj, MAX_OBJECTS - nobj);
    const struct rs_clip clip = { 0, 0, FB_W, car_cockpit() ? car_view_lines() : FB_H };
    volatile uint8_t *fb = FB(0);
    static struct blit_list back;
    blit_start(&back, 0);
    track_backdrop_blit(&back, &clip, &cam);
    blit_model(fb, &back);
    struct track_stats ts;
    track_mode = TRACK_MODEL;
    track_draw_cam(fb, &clip, &cam, objs, nobj, &ts);

    const uint32_t *pal = track_palette();
    FILE *f = fopen(argv[4], "wb");
    if (!f) { perror(argv[4]); return 1; }
    for (int i = 0; i < FB_W * FB_H; i++) {
        uint32_t c = pal[fb[i]];
        uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    for (int k = 0; k < opp_count(); k++) {
        float x, y;
        if (opp_where(k, &x, &y)) printf("opponent %d at %.1f %.1f\n", k, x, y);
    }
    printf("camera %.0f %.0f %.0f yaw %.4f pitch %.4f; %d pieces, %d faces, %d objects\n",
           cam.x, cam.y, cam.h, cam.yaw, cam.pitch, ts.pieces, ts.faces, nobj);
    return 0;
}
