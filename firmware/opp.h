/*
 * The opponents on the track: up to nine cars. Each drives its own curve of MAPnOPP.SPL at the
 * speed that its row of OPPONENTS allows on the stretch it is on, and stands on the road
 * under its middle. This module also keeps the running order between the opponents and the
 * player, and the contact between the cars: a car is pushed aside, slowed or crashes.
 * It follows OpenScreamer's port/src/main.c (GPL-3.0), which took it from the game
 * (openscreamer docs/NOTES.md section 11).
 */
#pragma once
#include <stdint.h>
#include "track.h"

#define OPP_MAX     9

/* Lines the opponents up on the grid (the start nodes of their curves), each in its car: the
   rows of CARS.DTR after the player's, which is player_row. */
void opp_init(int player_row);
/* 1: every opponent drives the Bullet (CARS.DTR row 12) in the paint of its own row, as in the
   game's difficulties above PRO (original: STARTL.EXE 0x14373: the championship's Bullet
   league). */
extern int opp_bullet;

/* One step of 1/50 s, after the player's (car_step). The opponents are held until the green
   light. */
void opp_step(void);

/* Puts the opponents into objs (up to max) as objects for track_draw_cam(), each followed by
   its shadow (SHADOW.3DX, efx.h, if loaded). Their blocks are made when they come up in the
   drawing. Returns how many objects. */
int opp_objects(struct track_object *objs, int max);

/* Where opponent k is (k is 0 .. opp_count() - 1). Returns 1 and puts its place on the ground
   into *x and *y, and its height into *z if z is given. Returns 0 for any other k. */
int opp_where(int k, float *x, float *y, float *z);

/* opp_count() is the number of opponents. opp_player_rank() is the player's place in the
   running order (1 = leading). */
int opp_count(void);
int opp_player_rank(void);

/* For the commentary (speech.h). opp_lapped() is 1 if the player is a lap up on one of the
   opponents: the player has more laps and is further round than that one, or has two laps more
   (original: STARTL.EXE 0x24464). opp_hitcar() is the player's contact with the opponents. It
   eases to 1 while the player touches one and to 0 when not (+0x7c4, stiffness 0x32: 0x175aa,
   0x177f7). */
int opp_lapped(void);
float opp_hitcar(void);
