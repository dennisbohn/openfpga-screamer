/*
 * The opponents on the track: up to nine cars, each driving its own curve of MAPnOPP.SPL at
 * the speed its row of OPPONENTS allows on the stretch it is on, standing on the road under
 * its middle, the running order between them and the player, and contact between the cars
 * (pushed aside, slowed, crashing). After OpenScreamer's port/src/main.c (GPL-3.0), which took
 * it from the game (docs/NOTES.md section 11).
 */
#pragma once
#include <stdint.h>
#include "track.h"

#define OPP_MAX     9

/* Line them up on the grid (the start nodes of their curves) in their cars: CARS.DTR's rows
   after the player's, which is player_row */
void opp_init(int player_row);
/* 1: every opponent drives the Bullet (CARS.DTR row 12) in its own row's paint, as the game's
   difficulties above PRO (0x14373: the championship's Bullet league) */
extern int opp_bullet;

/* One step of 1/50 s, after the player's (car_step): held until the green light */
void opp_step(void);

/* The opponents as objects for track_draw_cam() (up to max), each with its shadow after it
   (SHADOW.3DX, efx.h, if loaded), their blocks made when they come up in the drawing */
int opp_objects(struct track_object *objs, int max);

/* Where opponent k is (0 .. opp_count() - 1): 1 and its place on the ground */
int opp_where(int k, float *x, float *y);

/* How many there are; the player's place in the running order (1 = leading) */
int opp_count(void);
int opp_player_rank(void);

/* For the commentary (speech.h): the player is a lap up on one of them, more laps and further
   round this one or two laps more (0x24464); the player's contact with them, easing to 1 while
   touching one and to 0 when not (+0x7c4, stiffness 0x32: 0x175aa, 0x177f7) */
int opp_lapped(void);
float opp_hitcar(void);
