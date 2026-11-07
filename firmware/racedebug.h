/*
 * The debug build's measuring and logging of a race (debug.h). The race loop of main.c calls these functions
 * at the points of a picture where something is measured or logged:
 *   - the debug keys and the free camera: race_debug_keys, race_debug_camera (mark.h)
 *   - the timeline of the picture, a record of the cycles and of the units' busy clocks at each mark:
 *     tl_mark and the functions that mark by themselves
 *   - the figures of the picture: race_debug_measure reads the units' counters, race_debug_report adds the
 *     picture to the frame time statistics and overlay (ftime.h), logs the status block and the first line
 *     of debug.txt (log_race_head)
 *   - the benchmark: race_debug_bench (bench.h)
 * A release has none of this code. There every call below is an empty macro and race_dbg is a constant
 * that says that nothing is switched on, so that main.c reads the same in both builds.
 */
#pragma once
#include <stdint.h>
#include "debug.h"
#include "hw.h"
#include "track.h"

/* The most stack that has been used so far, in bytes (main.c). */
uint32_t stack_used(void);

/* The state that the race loop shares with the debug code. The first three members exist in a release too,
   where they are 0. The benchmark sets bench_frames before its races and clears it after them; the other
   members belong to the functions below. */
struct race_debug {
    int free_cam;                       /* the free camera is on (mark.h): the race stands still */
    uint32_t swallow;                   /* L and R held while A was held: they change no gear */
    uint32_t bench_frames;              /* in the benchmark's race: the pictures to count; else 0 */
#if DEBUG
    /* The race, set by race_debug_start. */
    int track;                          /* its number, for the marks */
    int sim;                            /* it runs in the simulation */
    uint32_t sim_race, check_at;        /* the simulation's long race (0 if not); the picture to check */
    uint8_t fg, bg;                     /* colours of the overlay's text, in the track's palette */
    /* What lasts from one picture to the next. */
    uint32_t fps, fps_drawn, fps_frame; /* pictures in the last second; the count and video frame then */
    int chord_before;                   /* A, L and R were all down in the picture before */
    uint32_t mark_show;                 /* pictures left to show the mark's number (mark.h) */
    int mark_no;                        /* that number; 0 if debug.txt had no room */
    struct track_cam free_c, last_cam;  /* the free camera; the camera the player's car had last */
    uint32_t steps;                     /* physics steps since the race began */
    uint32_t frame_cycles;              /* CPU cycles of the last picture, swap to swap */
    /* The picture in progress. */
    uint32_t drawn;                     /* its number */
    uint32_t t_frame;                   /* cycle count when it began */
    uint32_t t_phys;                    /* cycles of the physics (at first: where they began) */
    uint32_t t_scene, t_pre;            /* cycle count before the scene; cycles from t_frame up to it */
    uint32_t t_wait;                    /* cycle count where the wait for the rasterizer begins */
    uint32_t t_check;                   /* cycle count when the simulation's check began */
    /* What hw_piece measures. */
    int in_scene;                       /* the scene is drawn: hw_piece marks the timeline */
    uint32_t scene_pieces;              /* pieces handed over so far in this picture */
    uint32_t piece_t0;                  /* cycle count where the piece in hand began */
    uint32_t draw_wait;                 /* CPU cycles spent writing pieces to the geometry unit */
    uint32_t back_clocks, back_cycles;  /* the blitter's clocks for the backdrop; the CPU's wait for it */
    /* The units' counters, read by race_debug_measure when the picture is drawn. */
    struct {
        uint32_t blit, raster, pixels, misses, setup, geom;
        uint32_t r_miss, r_edge, r_row, r_drain, r_starved;     /* the rasterizer's clocks by kind of work */
        uint32_t cycles;                /* CPU cycles from just before the scene up to the end */
        uint32_t efx_objects, efx_radar;    /* the effects' cycles: shadows and smoke; radar (efx.h) */
    } units;
#endif
};

#if DEBUG

extern struct race_debug race_dbg;

/* The marks of the timeline. tl_mark stores a record with this tag. Most of the marks are set by the
   functions below, at the point of the picture that the tag names. */
enum { TL_START, TL_PHYS, TL_PREP, TL_CULL, TL_PIECES, TL_SCENE, TL_CLEAR, TL_HUD, TL_WAIT, TL_END,
       TL_MUSIC };
void tl_mark(uint32_t tag);

/* Once at the start of the program: keeps room in debug.txt for the marks (mark.h). */
void race_debug_init(void);

/* The race on track begins. sim is set in the simulation, sim_race is the number of pictures after the green
   light that the simulation's race runs (0 for an ordinary one), and picture check_at is the one that is
   checked against the C models. fg and bg are the colours of the overlay's text. */
void race_debug_start(int track, int sim, uint32_t sim_race, uint32_t check_at, uint8_t fg, uint8_t bg);

/* A picture begins, number drawn, at cycle count t_frame (after the swap of the picture before). Starts its
   timeline. One picture after the checked one it also writes the stack use to debug.txt. */
void race_debug_frame(uint32_t t_frame, uint32_t drawn);

/* The keys of the picture. A with L and R switches the overlay, A with X writes a mark and A with Y switches
   the free camera (mark.h). view is the car's view. *press may lose bits that these keys have taken: X
   while A is held, and START that gives the car's camera back. */
void race_debug_keys(uint32_t keys, uint32_t *press, int view);

/* The physics of the picture runs between these two calls. steps is the number it made. The end sets the
   timeline's mark TL_PHYS. */
void race_debug_physics_begin(void);
void race_debug_physics_end(uint32_t steps);

/* The camera cam of the picture is known. The debug build remembers it for the marks, and with the free
   camera on, replaces it by that camera, moved by the keys. */
void race_debug_camera(struct track_cam *cam, uint32_t keys);

/* The backdrop's blits are queued: clears the blitter's clocks and the backdrop's counters, so that they
   measure the backdrop alone. */
void race_debug_backdrop(void);

/* The scene is drawn between these two calls. The begin clears the units' counters, so that they cover this
   scene only, and sets the mark TL_PREP; during the scene the pieces set marks (piece_time_begin). The end
   sets the mark TL_SCENE. */
void race_debug_scene_begin(void);
void race_debug_scene_end(void);

/* The race screen is built and drawn between these two calls. The begin clears the blitter's clocks and sets
   the mark TL_CLEAR. The end sets the mark TL_HUD, where the wait for the rasterizer begins. */
void race_debug_screen_begin(void);
void race_debug_screen_end(void);

/* The rasterizer is done: sets the mark TL_WAIT, then reads the units' counters and puts the faces the
   geometry unit counted into ts. The simulation's check follows, which the report leaves out. */
void race_debug_measure(struct track_stats *ts);

/* The picture is finished. st is VID_STATUS from its start, fb the picture, ask tells whether it is the
   question's, steps_now the physics steps it made and ts its figures from the scene. Adds the picture to the
   statistics and the overlay, and writes the status block to debug.txt at the pictures that have it. */
void race_debug_report(uint32_t st, volatile uint8_t *fb, int ask, uint32_t steps_now,
                       const struct track_stats *ts);

/* Writes the race's first line of debug.txt: the pictures drawn so far and the frame times (ftime.h). */
void log_race_head(uint32_t n);

/* The benchmark, if screamer.cfg asks for it (bench.h). race is main.c's function that plays track n with
   car number car and returns 0 if the track could not be set up. */
void race_debug_bench(int (*race)(int n, int car));

/* What hw_piece measures for each piece it hands to the geometry unit: the timeline's marks at the first
   piece and at every eighth one, the cycles of the writes, and what the backdrop cost. They are inline,
   because hw_piece is hot. */
static inline void piece_time_begin(void)
{
    if (race_dbg.in_scene) {
        if (race_dbg.scene_pieces++ == 0)
            tl_mark(TL_CULL);
        else if (race_dbg.scene_pieces % 8 == 1)
            tl_mark(TL_PIECES);
    }
    race_dbg.piece_t0 = rdcycle();
}

/* The wait for the blitter's backdrop is over. */
static inline void piece_time_backdrop(void)
{
    race_dbg.back_clocks = BL_CLOCKS;
    race_dbg.back_cycles = rdcycle() - race_dbg.piece_t0;
    race_dbg.piece_t0 = rdcycle();
}

static inline void piece_time_end(void)
{
    race_dbg.draw_wait += rdcycle() - race_dbg.piece_t0;
}

#else

/* A release: nothing is measured. */
static const struct race_debug race_dbg = { 0, 0, 0 };

#define tl_mark(tag)                                    ((void)0)
#define race_debug_init()                               ((void)0)
#define race_debug_start(track, sim, sim_race, check_at, fg, bg)    ((void)0)
#define race_debug_frame(t_frame, drawn)                ((void)(t_frame))   /* t_frame has no other use */
#define race_debug_keys(keys, press, view)              ((void)0)
#define race_debug_physics_begin()                      ((void)0)
#define race_debug_physics_end(steps)                   ((void)0)
#define race_debug_camera(cam, keys)                    ((void)0)
#define race_debug_backdrop()                           ((void)0)
#define race_debug_scene_begin()                        ((void)0)
#define race_debug_scene_end()                          ((void)0)
#define race_debug_screen_begin()                       ((void)0)
#define race_debug_screen_end()                         ((void)0)
#define race_debug_measure(ts)                          ((void)0)
#define race_debug_report(st, fb, ask, steps_now, ts)   ((void)0)
#define log_race_head(n)                                ((void)(n))     /* the pictures are still counted */
#define race_debug_bench(race)                          ((void)0)
#define piece_time_begin()                              ((void)0)
#define piece_time_backdrop()                           ((void)0)
#define piece_time_end()                                ((void)0)

#endif
