/*
 * The opponents on the track: up to nine cars, each driving its own curve of MAPnOPP.SPL at
 * the speed its row of OPPONENTS allows on the stretch it is on, standing on the road under
 * its middle, and the running order between them and the player. After OpenScreamer's
 * port/src/main.c (GPL-3.0), which took it from the game (docs/NOTES.md section 11).
 */
#pragma once
#include <stdint.h>
#include "track.h"

#define OPP_MAX     9

/* Line them up on the grid (the start nodes of their curves) in their cars: CARS.DTR's rows
   after the player's, which is player_row */
void opp_init(int player_row);

/* One step of 1/50 s, after the player's (car_step): held until the green light */
void opp_step(void);

/* The opponents seen from cam through clip as objects for track_draw_cam() (up to max) */
int opp_objects(const struct rs_clip *clip, const struct track_cam *cam, struct track_object *objs, int max);

/* How many there are; the player's place in the running order (1 = leading) */
int opp_count(void);
int opp_player_rank(void);
