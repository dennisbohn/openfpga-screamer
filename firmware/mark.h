/*
 * Marks and the free camera (debug build; debug.h).
 *
 * A mark: in a race, hold A and press X (X alone changes the view). The camera's place goes to debug.txt, so
 * that one can find later on the PC what the picture did there, for example a building that is gone for a
 * frame. A press comes some time after what was seen, so a mark also has the camera of 15, 30 and 45 frames
 * before (at 50 frames a second: 0.3, 0.6 and 0.9 s). A mark is one line:
 *
 *   mark 3 track 4 view 2 frame 812: x y h yaw pitch roll | the same 15 frames earlier | 30 | 45
 *
 * x, y and h are in the game's units. yaw, pitch and roll are in 1/10000 radian, as in the other lines of the
 * log. View 9 is the free camera.
 *
 * The free camera: hold A and press Y. The race stands still (the cars, the clock and the scenery), and the
 * camera is the player's. The D-pad (or the stick) moves it forward and back along its view and turns it. L
 * and R lower and raise it, Y and X tilt it down and up, and B held makes everything fast. START gives the
 * car's camera back.
 */
#pragma once
#include <stdint.h>
#include "track.h"

#define MARK_RING   64              /* the cameras kept, one for each frame */
#define MARK_STEP   15              /* frames between the cameras that a mark writes */
#define MARK_POSES  4               /* the cameras that a mark writes: this frame's and three earlier ones */
#define MARK_BYTES  220             /* the room in debug.txt that a mark needs at most */
#define MARK_ROOM   1600            /* the room debug.txt keeps for marks (dlog_keep): about ten of them */

#define FREE_SPEED  160.0f          /* game units for each frame (picture) */
#define FREE_FAST   6.0f            /* the factor on the speed with B held */
#define FREE_TURN   0.025f          /* radians for each frame, for the yaw; the tilt uses 0.8 of it */
#define FREE_PITCH  1.45f           /* the tilt is kept within this, up and down */

/* Moves the free camera c for one picture. keys are the buttons held (a KEY_ word). The camera moves along
   the view's axes (track_cam_axes) and turns about the up axis; left turns the yaw up. */
void free_cam_step(struct track_cam *c, uint32_t keys);

void mark_reset(void);                              /* a new race */
void mark_push(const struct track_cam *cam);        /* each frame: remember the camera of the picture */
/* Writes a mark to debug.txt for the race on track, with the camera view view, at frame number frame.
   Returns the mark's number (1 and up), or 0 if debug.txt has no room for it; nothing is written then. */
int mark_save(int track, int view, uint32_t frame);
