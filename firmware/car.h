/*
 * The player's car on the track: the physics (drive.h) each 1/50 s step, standing on its
 * four wheels on the drivable surface, the lap's markers, and the camera that follows it.
 * The glue is OpenScreamer's (port/src/main.c: player_reset, player_contact, markers_step,
 * chase_step, view_pose), which took it from the game.
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

/* For the screen and the log */
const struct car_state *car_state(void);
