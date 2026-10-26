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

/* The car as an object for track_draw_cam(), its blocks in the game's order for the camera;
   0 when the view is from inside it */
int car_object(const struct track_cam *cam, struct track_object *obj);

/* For the screen and the log: the state, wall hits so far */
const struct car_state *car_state(void);
uint32_t car_wall_hits(void);
