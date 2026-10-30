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

/* the views the player can choose while driving, in the order the game's key goes round them
   (its table at 0x1da0, rows 0, 1, 2, 6): bumper cam, cockpit, near chase, far chase; a race
   starts in the near chase (the option at 0x1d2a4, 2 from 0x1bc50) */
#define CAR_VIEWS       4
#ifndef CAR_VIEW_START
#define CAR_VIEW_START  2
#endif

/* Set the car up from the track file: CARS.DTR row which, at the start; with replay for the
   race run again from its keys (the replay, 0x22d98): the trackside camera from the start, and
   the lap record kept as the race left it (0x20eb8 hands the best lap back) */
void car_init(int which, int replay);
int car_replay(void);

/* The controls a step reads, made of the keys (IN_KEY) held now and on the step before: B, Y,
   left and right as they are held, L and R only on the step they went down; and of a docked
   controller's analog parts (IN_JOY, IN_TRIG; hw.h): the left stick across and the two
   triggers, 0x80, 0, 0 without one. A race recorded as these, one a step, runs again the same
   from them (the game records its controls, 0x22f34). */
struct car_input { uint16_t keys; uint8_t stick, brake, gas; };
struct car_input car_keys(uint32_t keys, uint32_t keys_before, uint32_t joy, uint32_t trig);
/* One step of 1/50 s with the controls of car_keys() */
void car_step(const struct car_input *in);

/* Choose the view (0 .. CAR_VIEWS - 1); the camera it gives now. From the race's end on the
   camera is the trackside one (view-table row 3, 0x1a620), the car and its shadow in view. */
void car_view(int view);
void car_camera(struct track_cam *cam);
/* The view's 3D picture: its lines from the top (the table's third column: 132 in the cockpit,
   200 else; 0x19cbe sets the view up as 0x4c610(320, lines, lines x 0.65)); its overlay (the
   fourth column, hud.h: 1 the dashboard over the rest, 2 the outside views' instruments, 0
   neither: the trackside camera), and 1 if it is the dashboard */
int car_view_lines(void);
int car_overlay(void);
int car_cockpit(void);
/* The trackside cameras' places: the nodes of MAPnRPL.SPL from track n's DATA container dat
   (as the data slot loaded it); without them the race's end keeps its view */
void car_trackside_init(const uint8_t *dat, uint32_t dat_size, int n);
/* The trackside camera from now on, as the race's end asks for it (car_step) */
void car_trackside(void);

/* The car as an object for track_draw_cam(), its blocks made in the game's order for the
   camera when it comes up in the drawing; 0 when the view is from inside it */
int car_object(const struct track_cam *cam, struct track_object *obj);
/* the car object's reflection accumulators and picture set (the original's record +0x30 and
   +0x3c, for tools/render.sh), the car standing where it is */
void car_reflection(const float acc[3], uint32_t phase);

/* Any car as an object: model AUTOnn (car_model_index of its CARS.DTR row) drawn at place
   x, y, z, heading yaw, the object's pitch and roll (the car's negated), its body's own lean
   on top (not the wheels'), the wheels' turn and the front ones' angle, the texture phase
   (geom.h). Its blocks go into out (CAR_BLOCKS), in the game's order for the camera. */
#define CAR_BLOCKS  16
struct car_look { float x, y, z, yaw, pitch, roll, body_pitch, body_roll, spin, steer; uint32_t phase; };
int car_model_index(const struct car_spec *sp);
int car_draw(int model, const struct car_look *lk, const struct track_cam *cam, struct track_block *out,
             struct track_object *obj);
/* The same for a car's mesh that is not the track's (the car screen's AAUTOnn.3DX, menu.h): its
   blocks into out (CAR_BLOCKS) in the game's order for the camera; how many */
int car_mesh_draw(const uint8_t *mesh, const struct car_look *lk, const struct track_cam *cam,
                  struct track_block *out);
/* the player's car as car_object() draws it now (time attack records it, ghost.h) */
void car_player_look(struct car_look *lk);

/* The first block of a flat mesh (a shadow) placed as a car's body is: at x, y, z, heading yaw,
   tilted by pitch and roll (struct car_look's) */
void car_flat_block(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    struct track_block *out);
/* Every block of a mesh placed so (the helicopter: its body and its rotor), each at its own
   place in the mesh; how many went into out, up to max */
int car_mesh_blocks(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    struct track_block *out, int max);

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
   lap time from the first crossing of the line; car_laps laps; time over once the countdown is
   out and the car has stopped */
struct car_race {
    float clock, timer, total, lap, best, last;
    int laps, total_laps, on, clock_on, time_out, finished, game_over;
    int perfect;                /* PERFECT! at the finish (flag 0x8000): cone carnage every prop down,
                                   slalom every gate and no prop touched (0x23fbc, 0x23f68) */
    int made_it;                /* the special modes' finish counts (flag 0x800): always in cone
                                   carnage and slalom, in time attack faster than the ghost or
                                   without one (0x23ef0); YOU MADE IT!, else TRY AGAIN */
};
const struct car_race *car_race(void);
/* the laps of a race: the options page's LAP NUMBER (menu.h, 0x1d2ac + 1), 3 at first */
extern int car_laps;

/* The game mode, the game's word 0x1d25c as the menu's items set it (0x28b80 ..). In the special
   modes (CAR_ALONE) the player races alone (0x1311c, ...: no opponents), one lap (0x2a216), from
   the last opponent's place on the grid (0x13ded) and with no lap to beat (0x2a264: 0). In cone
   carnage and slalom the line and the checkpoints give TRACK_DATA's fourth number, the carnage
   time, instead of the bonus (0x195fc); cone carnage's props come from MAPnCNG.SPL (scenery.h)
   and each step in which more of them went down than before adds 1 s (0x18b50); slalom's from
   MAPnSLA.SPL, and the first time through each gate of MAPnSLA.MRK (a circle between two of
   them) adds its seconds, with the door's sample (0x18b98). */
#define CAR_NORMAL  1
#define CAR_CHAMP   2
#define CAR_ATTACK  4
#define CAR_NETWORK 0x10
#define CAR_CONES   0x20
#define CAR_SLALOM  0x40
#define CAR_ALONE   (CAR_ATTACK | CAR_CONES | CAR_SLALOM)
extern int car_mode;
/* The mode's numbers of track n from its DATA container dat: the carnage time of TRACK_DATA in
   the difficulty's MAPn?.DTR (0 ROOKIE = A, as track_build.h), slalom's gates; before car_init() */
void car_mode_init(const uint8_t *dat, uint32_t dat_size, int n, int difficulty);
/* slalom: the gates driven through so far; how many there are into *total (if given) */
int car_gates(int *total);
/* the lap to beat (0x1d268), the race screen's third clock: in the normal game and the
   championship car_lap_to_beat, none (0) in the special modes but time attack's ghost's lap
   (ghost.h); a lap under it replaces it with "new lap record"; at the finish the race's best lap
   (0x20eb8) */
float car_lap_record(void);
/* what car_init() takes as the lap to beat of the normal game and the championship: the caller
   sets the track's best lap of the records before the first race (0x2a264: CHOICE.DAT's
   0x2042c + 0x50 x track) and car_lap_record() after it, which PLAY AGAIN starts with */
extern float car_lap_to_beat;
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
/* the car heads within 45 degrees of the road reversed, the road's heading where it last met a
   wall (0x2425c, the commentator's "back"; the start's heading until then) */
int car_backwards(void);
/* the rev counter's needle: rpm / maxrpm x pi on the outside views' instrument, x 3.613 on
   the cockpit's dashboard; the dashboard's speed needle, speed / maxspd x 4.084; both eased
   (dash_step) */
float car_needle(void);
float car_speed_needle(void);

/* For the screen and the log: the state, wall hits and crashes so far, the marker last
   reached */
const struct car_state *car_state(void);
uint32_t car_wall_hits(void);
uint32_t car_crashes(void);
int car_marker(void);
