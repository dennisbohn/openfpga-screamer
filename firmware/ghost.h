/*
 * Time attack's ghost (the game's mode 4, car.h; openscreamer docs/NOTES.md section 20). The
 * game records the player's run (0x231d0), keeps a lap that beat the last one (or the first) as
 * REPLAYS\MAPn.ATK (0x1fd64 -> 0x1b964) and, after RESUME or PLAY AGAIN, races it as a second car
 * that nothing collides with (0x12e14 -> 0x1ba3c, 0x14d28; played back by 0x23018 and 0x23308):
 * the recorded car model on the grid place it was recorded from, the player on the other one
 * (0x13ded, 0x1bb33), its lap the one to beat (0x1d268), the player second while it is further
 * round (0x20418). At the finish (0x23ef0) a lap slower than the ghost's, or as slow, is TRY
 * AGAIN; else YOU MADE IT! and the run is kept.
 *
 * Here: a pose every GHOST_STEP physics steps from the set-up on (the game's are one a frame),
 * played back through the original's cubic (0x22724) at the same step, the angles turned
 * between two poses; each track's best kept in the save file ATTACK.DAT (save.h).
 */
#pragma once
#include <stdint.h>
#include "track.h"

#define GHOST_STEP      4       /* a pose every 4 steps of 1/50 s: 12.5 a second */
#define GHOST_MAX       3000    /* poses: 240 s from the set-up */
#define GHOST_TRACKS    6

/* The save file's layout, little-endian (one per track, the game's REPLAYS\MAPn.ATK): 16 bytes
   of header, then n poses of 20 bytes; 16 + 20 x n bytes are used. A pose: the car's place in
   whole units, its heading, pitch and roll as drawn (65536 = 2 pi), and in info its progress
   round the race (car_progress) x 64 in bits 0-12, bits 13 and 14 the texture frame's "slow"
   and "not braking" (0x2000, 0x4000, car_draw's phase << 2). */
struct ghost_pose { int32_t x, y, z; int16_t yaw, pitch, roll; uint16_t info; };
struct ghost_file {
    char magic[4];              /* "ATK1"; anything else: no run kept */
    uint8_t track, car;         /* 1 .. 6; the car's row of CARS.DTR */
    uint16_t n;                 /* poses */
    uint32_t steps;             /* the lap's time in steps of 1/50 s */
    uint32_t flags;             /* bit 0: the grid place it was recorded from (car.h) */
    struct ghost_pose pose[GHOST_MAX];
};

/* RESUME on the menu's page 3 (0x28c3c): race the track's kept run; NEW (0x28c0f): alone. A run
   kept sets it (0x1fded), so PLAY AGAIN races it. */
extern int ghost_resume;

/* Track n's set-up for the car of CARS.DTR row car, before car_init(): the ghost, in time attack
   with ghost_resume and a kept run of track n, and an empty recording */
void ghost_init(int n, int car);
/* the player's grid place in time attack: 0 the last opponent's, 1 the one after (the other
   one than the ghost's); the ghost's lap in seconds, 0 without one */
int ghost_slot(void);
float ghost_time(void);
/* One step, at the end of car_step(): the player's pose recorded every GHOST_STEP steps */
void ghost_step(void);
/* At the finish with the lap's time: 1 if it counts (no ghost, or faster than it: 0x23ef0),
   then the run is kept once the race has ended (ghost_end) */
int ghost_finish(float total);
/* The race ended (PLAY AGAIN? answered): a run that counted becomes the track's best */
void ghost_end(void);
/* 1 while the ghost is further round than the player (the running order, 0x20418) */
int ghost_ahead(void);
/* The ghost as an object for track_draw_cam() (up to max), its blocks made when it comes up in
   the drawing */
int ghost_objects(struct track_object *objs, int max);
/* Track n's kept run (1 .. GHOST_TRACKS), for saving and loading; 0 for another n */
struct ghost_file *ghost_track(int n);
