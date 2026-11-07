/*
 * The player's car on the track (car.c). Every step of 1/50 s it runs the physics (drive.h),
 * keeps the car inside the walls, stands it on its four wheels on the drivable surface, follows
 * the lap's markers and the race clocks, and moves the camera that follows the car. It also
 * makes the car itself, an object of up to CAR_BLOCKS blocks, for the drawing.
 * The logic is OpenScreamer's (port/src/main.c: player_reset, player_walls, player_contact,
 * markers_step, chase_step, view_pose, wheels_step, draw_object), which took it from the game.
 * Addresses in the form (original: STARTL.EXE 0x...) are places in the original's executable.
 */
#pragma once
#include <stdint.h>
#include "drive.h"
#include "track.h"

/* The views the player can choose while driving, in the order the game's key goes round them:
   bumper cam, cockpit, near chase, far chase. They are rows 0, 1, 2 and 6 of the game's view
   table (original: STARTL.EXE 0x1da0). A race starts in the near chase (original: STARTL.EXE
   0x1d2a4, the option, with its value 2 from 0x1bc50). A build can set CAR_VIEW_START to start
   in another view. */
#define CAR_VIEWS       4
#ifndef CAR_VIEW_START
#define CAR_VIEW_START  2
#endif

/* Sets the car up at the start of a race, from row `which` of CARS.DTR in the track file.
   With `replay` set the race runs again from its recorded keys (the replay,
   original: STARTL.EXE 0x22d98): the trackside camera is on from the start, and the lap record
   stays as the race left it (original: STARTL.EXE 0x20eb8 hands the best lap back). */
void car_init(int which, int replay);
int car_replay(void);

/* The controls that one step reads. car_keys() makes them from the keys (IN_KEY) held now and
   on the step before: B, Y, left and right as they are held, L and R only on the step they
   went down. A docked controller adds its analog parts (IN_JOY, IN_TRIG; hw.h): the left stick
   across, and the two triggers. Without a controller these are 0x80, 0 and 0. A race recorded
   as these controls, one per step, runs again exactly the same; the game also records its
   controls (original: STARTL.EXE 0x22f34). */
struct car_input { uint16_t keys; uint8_t stick, brake, gas; };
struct car_input car_keys(uint32_t keys, uint32_t keys_before, uint32_t joy, uint32_t trig);
/* One step of 1/50 s with the controls that car_keys() made. */
void car_step(const struct car_input *in);

/* Chooses the view (0 .. CAR_VIEWS - 1). car_camera() gives the camera of the view as it is
   now. From the race's end on the camera is the trackside one, which keeps the car and its
   shadow in view: row 3 of the view table (original: STARTL.EXE 0x1a620). */
void car_view(int view);
void car_camera(struct track_cam *cam);
/* The 3D picture of the view, as the view table has it:
   - car_view_lines(): its number of lines from the top, 132 in the cockpit and 200 in the other
     views. The game sets the view up as 0x4c610(320, lines, lines x 0.65)
     (original: STARTL.EXE 0x19cbe).
   - car_overlay(): what is drawn over it, the table's fourth column (hud.h): 1 the dashboard
     over the rest, 2 the instruments of the outside views, 0 neither (the trackside camera).
   - car_cockpit(): 1 if the view is the dashboard. */
int car_view_lines(void);
int car_overlay(void);
int car_cockpit(void);
/* Reads the places of the trackside cameras: the nodes of MAPnRPL.SPL in the DATA container dat
   of track n, as the data slot loaded it. Without them the race's end keeps its view. */
void car_trackside_init(const uint8_t *dat, uint32_t dat_size, int n);
/* Turns the trackside camera on from now on. car_step() calls it at the race's end. */
void car_trackside(void);

/* The car as an object for track_draw_cam(). Its blocks are made, in the game's order for the
   camera, when the object comes up in the drawing. Returns 0 when the view is from inside the
   car. */
int car_object(const struct track_cam *cam, struct track_object *obj);
/* Sets the reflection state of the car object: the accumulators acc and the set of pictures
   phase (the original's record +0x30 and +0x3c). tools/render.sh uses it. The object stands
   where the car is. */
void car_reflection(const float acc[3], uint32_t phase);

/* Draws any car as an object: model AUTOnn (nn is car_model_index() of its CARS.DTR row) at
   place x, y, z with heading yaw. pitch and roll are the object's, which are the car's negated.
   The body's own lean (body_pitch, body_roll; not the wheels') comes on top. spin is the
   wheels' turn, steer the angle of the front wheels, phase the texture phase (geom.h).
   The blocks go into out (CAR_BLOCKS at most) in the game's order for the camera. */
#define CAR_BLOCKS  16
struct car_look { float x, y, z, yaw, pitch, roll, body_pitch, body_roll, spin, steer; uint32_t phase; };
int car_model_index(const struct car_spec *sp);
int car_draw(int model, const struct car_look *lk, const struct track_cam *cam, struct track_block *out,
             struct track_object *obj);
/* The same for a car's mesh that is not the track's, such as the car screen's AAUTOnn.3DX
   (menu.h). The blocks go into out (CAR_BLOCKS at most) in the game's order for the camera.
   Returns how many. */
int car_mesh_draw(const uint8_t *mesh, const struct car_look *lk, const struct track_cam *cam,
                  struct track_block *out);
/* Gives the look of the player's car as car_object() draws it now (time attack records it,
   ghost.h). */
void car_player_look(struct car_look *lk);

/* Makes the first block of a flat mesh (a shadow) and places it as a car's body is placed: at
   x, y, z, heading yaw, tilted by pitch and roll (as in struct car_look). */
void car_flat_block(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    struct track_block *out);
/* Places every block of a mesh as one body (the helicopter: its body and its rotor), each at
   its own place in the mesh. Returns how many blocks went into out, up to max.
   With a camera the blocks come in the game's drawing order for it: the block whose middle is
   furthest away comes first (original: STARTL.EXE 0x8ac58), so the rotor, which is above the
   body, goes first when the camera is below. Without a camera they stay in the mesh's order. */
int car_mesh_blocks(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    const struct track_cam *cam, struct track_block *out, int max);

/* The player's shadow (SHADOW.3DX, efx.h) and tyre smoke (SMOKE.3DX, up to 8 puffs) as objects
   for track_draw_cam(), up to max of them. Their blocks are made when they come up in the
   drawing. The camera cam moves the shadow a little. Returns how many objects there are; there
   are none without the meshes. */
int car_effects(const struct track_cam *cam, struct track_object *objs, int max);
/* car_puffs(): how many puffs there are now. car_shadow_at(): the shadow's place; returns 0 if
   there is none (the bumper cam). */
int car_puffs(void);
int car_shadow_at(float *x, float *y, float *h);

/* The race clocks and flags (original: STARTL.EXE 0x18a04, 0x1820c, 0x19490, 0x20d45;
   OpenScreamer's markers_step).
   - clock: seconds since the start. The start lights go green at 6 s, and `on` is set.
   - timer: the countdown, from 15 s. At the line and at each checkpoint it becomes what is left
     / 5 + the bonus of TRACK_DATA.
   - total, lap: the total time and the lap time, from the first crossing of the line.
   - laps, total_laps: laps done and laps of the race (car_laps).
   - game_over: time is over, once the countdown is out and the car has stopped. */
struct car_race {
    float clock, timer, total, lap, best, last;
    int laps, total_laps, on, clock_on, time_out, finished, game_over;
    /* perfect: PERFECT! at the finish (flag 0x8000). In cone carnage it means every prop is
       down; in slalom, every gate was driven through and no prop was touched
       (original: STARTL.EXE 0x23fbc, 0x23f68). */
    int perfect;
    /* made_it: the finish of a special mode counts (flag 0x800). It always does in cone
       carnage and slalom; in time attack only if the race was faster than the ghost's, or there
       was no ghost (original: STARTL.EXE 0x23ef0). It selects YOU MADE IT!, else TRY AGAIN. */
    int made_it;
};
const struct car_race *car_race(void);
/* The laps of a race: the options page's LAP NUMBER (menu.h, original: STARTL.EXE 0x1d2ac + 1).
   It is 3 at first. */
extern int car_laps;

/* The game mode is the game's word at original: STARTL.EXE 0x1d25c. The menu's items set it
   (original: STARTL.EXE 0x28b80 and following).
   The special modes (CAR_ALONE) change the race:
   - The player races alone, with no opponents (original: STARTL.EXE 0x1311c and others), for
     one lap (0x2a216), from the last opponent's place on the grid (0x13ded), and with no lap
     to beat (0x2a264: 0).
   - In cone carnage and slalom the line and the checkpoints give the fourth number of
     TRACK_DATA, the carnage time, instead of the bonus (original: STARTL.EXE 0x195fc).
   - Cone carnage: the props come from MAPnCNG.SPL (scenery.h). Each step in which more of them
     went down than before adds 1 s (original: STARTL.EXE 0x18b50).
   - Slalom: the props come from MAPnSLA.SPL. The first time through each gate of MAPnSLA.MRK
     (a circle between two of the props) adds the gate's seconds and plays the door's sample
     (original: STARTL.EXE 0x18b98). */
#define CAR_NORMAL  1
#define CAR_CHAMP   2
#define CAR_ATTACK  4
#define CAR_NETWORK 0x10
#define CAR_CONES   0x20
#define CAR_SLALOM  0x40
#define CAR_INTRO   0x100       /* the intro's race, no player (intro.h; original: STARTL.EXE 0x277d5) */
#define CAR_ALONE   (CAR_ATTACK | CAR_CONES | CAR_SLALOM)
extern int car_mode;
/* Sets up the intro's race (intro.h). Call it after car_init(). The race is on at once (flags
   0x2001, no start lights). The player's car is put out of the world, as the intro has none
   (original: STARTL.EXE 0x2780c steps only the opponents), and its engine is silent.
   car_step() is not called in the intro. */
void car_intro(void);
/* Reads the numbers of the game mode for track n from its DATA container dat: the carnage time
   of TRACK_DATA in the MAPn?.DTR of the difficulty (0 ROOKIE = A, as in track_build.h), and
   slalom's gates. Call it before car_init(). */
void car_mode_init(const uint8_t *dat, uint32_t dat_size, int n, int difficulty);
/* Slalom: returns the gates driven through so far. The number of gates goes into *total, if
   total is given. */
int car_gates(int *total);
/* The lap to beat (original: STARTL.EXE 0x1d268), shown as the race screen's third clock.
   - Normal game and championship: car_lap_to_beat.
   - Special modes: none (0), except in time attack, where it is the ghost's lap (ghost.h).
   A lap under it replaces it and shows "new lap record". At the finish it becomes the race's
   best lap (original: STARTL.EXE 0x20eb8). */
float car_lap_record(void);
/* What car_init() takes as the lap to beat in the normal game and the championship. Before the
   first race the caller sets it to the track's best lap of the records (original: STARTL.EXE
   0x2a264: CHOICE.DAT's 0x2042c + 0x50 x track). After a race it is car_lap_record(), which
   PLAY AGAIN starts with. */
extern float car_lap_to_beat;
/* Contact with the opponents (opp.c; the game's routine is original: STARTL.EXE 0x17818).
   - car_circles(): the car's two circles (original: STARTL.EXE 0x136c8). The body reaches from
     250 ahead of the front wheels to 150 behind the rear ones, and has the length L. Each circle
     has the radius L/4, and its centre lies L/4 in from its end. Returns the radius.
   - car_shift(): moves the car out of an opponent's way when the opponent could not be moved
     itself.
   - car_rammed(): the car ran into the opponent in front. It is shoved back along (nx, ny) at
     0.21 x the speed and its nose is turned away (left: the car is on the other's left). It
     loses 5 % of the speed in each step of contact, and each step counts as a wall hit. */
float car_circles(float cx[2], float cy[2]);
void car_shift(float dx, float dy);
void car_rammed(float nx, float ny, int left);

/* How far round the race a car is: laps x 100 + the marker last reached + the share of the way
   to the next (original: STARTL.EXE 0x1820c). car_progress() is the player's;
   car_marker_progress() takes any car at (x, y) with its marker `at` and its laps. */
float car_progress(void);
float car_marker_progress(int at, int laps, float x, float y);
/* 1 if the car heads within 45 degrees of the road reversed. The road's heading is that of the
   nearest road point (original: STARTL.EXE 0x2425c, the commentator's "back"); until the first
   step it is the start's heading. */
int car_backwards(void);
/* The needle of the rev counter: rpm / maxrpm x pi on the instrument of the outside views, and
   x 3.613 on the cockpit's dashboard. car_speed_needle() is the dashboard's speed needle,
   speed / maxspd x 4.084. Both are eased (dash_step in car.c). */
float car_needle(void);
float car_speed_needle(void);

/* For the screen and the log: the car's state, the wall hits and crashes so far, and the marker
   last reached. */
const struct car_state *car_state(void);
uint32_t car_wall_hits(void);
uint32_t car_crashes(void);
int car_marker(void);
