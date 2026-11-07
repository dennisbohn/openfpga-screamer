/*
 * Marks: in a race, hold A and press X (X alone changes the camera) to write the camera's place to debug.txt, for finding
 * what the picture did there later on the PC (a building that is gone for a frame). A press comes some time after what was
 * seen, so a mark also has the camera of 15, 30 and 45 frames before (at 50 frames a second: 0.3, 0.6 and 0.9 s).
 *
 *   mark 3 track 4 view 2 frame 812: x y h yaw pitch roll | (15 frames earlier) | (30) | (45)
 *
 * x, y, h in the game's units; yaw, pitch, roll in 1/10000 radian (as the other lines of the log have them). View 9 is
 * the free camera.
 *
 * The free camera: hold A and press Y. The race stands still (the cars, the clock, the scenery), the camera is the player's:
 * the D-pad (or the stick) moves it forward and back along its view and turns it, L and R lower and raise it, Y and X
 * tilt it down and up, B held makes everything fast; START gives the car's camera back.
 */
#pragma once
#include <stdint.h>
#include "track.h"

#define MARK_RING   64              /* the cameras kept, one a frame */
#define MARK_STEP   15              /* frames between the cameras a mark writes */
#define MARK_POSES  4               /* the cameras a mark writes: now and three earlier ones */
#define MARK_BYTES  220             /* debug.txt's room a mark needs at most */

#define FREE_SPEED  160.0f          /* units a frame (a picture) */
#define FREE_FAST   6.0f            /* times that with B held */
#define FREE_TURN   0.025f          /* radians a frame, yaw; the tilt takes 0.8 of it */
#define FREE_PITCH  1.45f           /* the tilt is kept within this, up and down */

/* One picture of the free camera c: the buttons held (keys, a KEY_ word) move it. Moving is along the view's axes
   (track_cam_axes), the turns about the up axis, left = yaw up */
void free_cam_step(struct track_cam *c, uint32_t keys);

void mark_reset(void);                              /* a new race */
void mark_push(const struct track_cam *cam);        /* each frame: the camera of the picture */
/* Write a mark for the race on track, camera view view, frame number frame to debug.txt: its number (1 ..), or 0 if
   debug.txt has no room for it (nothing is written then) */
int mark_save(int track, int view, uint32_t frame);
