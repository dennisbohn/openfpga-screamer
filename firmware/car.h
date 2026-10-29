/*
 * The player's car on the track: the physics (drive.h) each 1/50 s step, the walls,
 * standing on its four wheels on the drivable surface, the lap's markers, the camera that
 * follows it, and the car itself as an object of twelve blocks for the drawing. The glue is
 * OpenScreamer's (port/src/main.c: player_reset, player_walls, player_contact, markers_step,
 * chase_step, view_pose, wheels_step, draw_object), which took it from the game.
 */
#pragma once
#include <stdint.h>
#include "drive.h"
#include "track.h"

/* the views the player can choose while driving (the game's table at 0x1da0, rows 2, 6, 0):
   near chase (where a race starts), far chase, bumper cam */
#define CAR_VIEWS   3

/* Set the car up from the track file: CARS.DTR row which, at the start */
void car_init(int which);

/* One step of 1/50 s with the keys (IN_KEY) held now and on the step before */
void car_step(uint32_t keys, uint32_t keys_before);

/* Choose the view (0 .. CAR_VIEWS - 1); the camera it gives now */
void car_view(int view);
void car_camera(struct track_cam *cam);

/* The car as an object for track_draw_cam(), its blocks made in the game's order for the
   camera when it comes up in the drawing; 0 when the view is from inside it */
int car_object(const struct track_cam *cam, struct track_object *obj);

/* Any car as an object: model AUTOnn (car_model_index of its CARS.DTR row) drawn at place
   x, y, z, heading yaw, the object's pitch and roll (the car's negated), its body's own lean
   on top (not the wheels'), the wheels' turn and the front ones' angle, the texture phase
   (geom.h). Its blocks go into out (CAR_BLOCKS), in the game's order for the camera. */
#define CAR_BLOCKS  16
struct car_look { float x, y, z, yaw, pitch, roll, body_pitch, body_roll, spin, steer; uint32_t phase; };
int car_model_index(const struct car_spec *sp);
int car_draw(int model, const struct car_look *lk, const struct track_cam *cam, struct track_block *out,
             struct track_object *obj);

/* The first block of a flat mesh (a shadow) placed as a car's body is: at x, y, z, heading yaw,
   tilted by pitch and roll (struct car_look's) */
void car_flat_block(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    struct track_block *out);

/* The player's shadow (SHADOW.3DX, efx.h) and tyre smoke (SMOKE.3DX, up to 8 puffs) as objects
   for track_draw_cam() (up to max), their blocks made when they come up in the drawing; the
   camera cam nudges the shadow. None of them without the meshes. */
int car_effects(const struct track_cam *cam, struct track_object *objs, int max);
/* how many puffs there are now, the shadow's place (0 if none: the bumper cam) */
int car_puffs(void);
int car_shadow_at(float *x, float *y, float *h);

/* The race (0x18a04, 0x1820c, 0x19490, 0x20d45; OpenScreamer's markers_step): the clock
   since the start, whose start lights go green at 6 s; the countdown from 15 s, which at the
   line and at each checkpoint becomes what is left / 5 + TRACK_DATA's bonus; the total and
   lap time from the first crossing of the line; three laps; time over once the countdown is
   out and the car has stopped */
struct car_race {
    float clock, timer, total, lap, best, last;
    int laps, total_laps, on, clock_on, time_out, finished, game_over;
};
const struct car_race *car_race(void);
/* Contact with the opponents (opp.c, the game's 0x17818): the car's two circles (0x136c8:
   the body from 250 ahead of the front wheels to 150 behind the rear ones, length L, each
   circle L/4 in from its end and L/4 round; returns the radius); moved out of an opponent's
   way when it could not be moved itself; ran into the opponent in front: shoved back along
   (nx, ny) at 0.21 x the speed, the nose turned away (left: the car is on the other's left),
   5 % of the speed lost each step of contact, counted as a wall hit */
float car_circles(float cx[2], float cy[2]);
void car_shift(float dx, float dy);
void car_rammed(float nx, float ny, int left);

/* how far round the race: laps x 100 + the marker last reached + the share of the way to the
   next (0x1820c), for the player and for any car at (x, y) */
float car_progress(void);
float car_marker_progress(int at, int laps, float x, float y);
/* the outside views' rev counter needle: rpm / maxrpm x pi, eased (dash_step) */
float car_needle(void);

/* For the screen and the log: the state, wall hits and crashes so far, the marker last
   reached */
const struct car_state *car_state(void);
uint32_t car_wall_hits(void);
uint32_t car_crashes(void);
int car_marker(void);
