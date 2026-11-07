/*
 * Time attack's ghost (the game's mode 4, car.h; openscreamer docs/NOTES.md section 20).
 * The game's rules:
 *   - It records the player's run (original: STARTL.EXE 0x231d0). It keeps a lap that beat the
 *     last one, or the first lap, as REPLAYS\MAPn.ATK (0x1fd64 -> 0x1b964).
 *   - After RESUME or PLAY AGAIN it races the kept run as a second car that nothing collides
 *     with (0x12e14 -> 0x1ba3c, 0x14d28; played back by 0x23018 and 0x23308). The ghost is the
 *     recorded car model on the grid place it was recorded from. The player takes the other
 *     place (0x13ded, 0x1bb33).
 *   - The ghost's lap is the lap to beat (0x1d268). The player is second while the ghost is
 *     further round (0x20418).
 *   - At the finish (0x23ef0) a lap slower than the ghost's, or as slow, is TRY AGAIN.
 *     Otherwise it is YOU MADE IT! and the run is kept.
 *
 * In this core a pose is recorded every GHOST_STEP physics steps from the set-up on (the
 * game's poses are one a frame). The playback goes through the original's cubic (0x22724) at
 * the same step, and the angles are turned between two poses. Each track's best run is kept in
 * the save file ATTACK.DAT (save.h).
 */
#pragma once
#include <stdint.h>
#include "track.h"

#define GHOST_STEP      4       /* a pose every 4 steps of 1/50 s: 12.5 a second */
#define GHOST_MAX       3000    /* poses: 240 s from the set-up */
#define GHOST_TRACKS    6

/*
 * The save file's layout, little-endian. There is one run for each track, in place of the
 * game's REPLAYS\MAPn.ATK. It has 16 bytes of header and then n poses of 20 bytes; the file
 * uses 16 + 20 x n bytes.
 * A pose holds the car's place in whole units and its heading, pitch and roll as drawn
 * (65536 = 2 pi). Its field info holds the car's progress round the race (car_progress) x 64
 * in bits 0-12. Bits 13 and 14 are the texture frame's "slow" and "not braking" (0x2000,
 * 0x4000; car_draw's phase << 2).
 */
struct ghost_pose { int32_t x, y, z; int16_t yaw, pitch, roll; uint16_t info; };
struct ghost_file {
    char magic[4];              /* "ATK1"; anything else: no run kept */
    uint8_t track, car;         /* 1 .. 6; the car's row of CARS.DTR */
    uint16_t n;                 /* poses */
    uint32_t steps;             /* the lap's time in steps of 1/50 s */
    uint32_t flags;             /* bit 0: the grid place it was recorded from (car.h) */
    struct ghost_pose pose[GHOST_MAX];
};

/* RESUME on page 3 of the menu (original: STARTL.EXE 0x28c3c) races the track's kept run; NEW
   (0x28c0f) races alone. A run that is kept sets this (0x1fded), so PLAY AGAIN races it. */
extern int ghost_resume;

/* Sets up track n for the car of CARS.DTR row car, before car_init(). In time attack with
   ghost_resume and a kept run of track n there is a ghost. The recording starts empty. */
void ghost_init(int n, int car);
/* ghost_slot() is the player's grid place in time attack: 0 for the last opponent's, 1 for
   the one after it (the other one than the ghost's). ghost_time() is the ghost's lap in
   seconds, 0 without a ghost. */
int ghost_slot(void);
float ghost_time(void);
/* One step, at the end of car_step(): records the player's pose every GHOST_STEP steps. */
void ghost_step(void);
/* At the finish, with the lap's time. Returns 1 if the lap counts (no ghost, or faster than
   it: original 0x23ef0). Then the run is kept once the race has ended (ghost_end). */
int ghost_finish(float total);
/* The race has ended (PLAY AGAIN? is answered): a run that counted becomes the track's best. */
void ghost_end(void);
/* Returns 1 while the ghost is further round than the player (the running order, original:
   STARTL.EXE 0x20418). */
int ghost_ahead(void);
/* Puts the ghost into objs (up to max) as an object for track_draw_cam(). Its blocks are made
   when it comes up in the drawing. Returns how many objects. */
int ghost_objects(struct track_object *objs, int max);
/* Returns track n's kept run (n is 1 .. GHOST_TRACKS), for saving and loading, or 0 for any
   other n. */
struct ghost_file *ghost_track(int n);
