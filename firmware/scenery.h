/*
 * The scenery that is not the track's (openscreamer docs/NOTES.md section 14):
 *
 * Props (the game's 0x15178 places them, 0x124c0 hits one, 0x205b4 moves it): one per node of
 * the track's MAPnOST.SPL (cone carnage MAPnCNG.SPL, slalom MAPnSLA.SPL: 0x14fb4), the node's
 * height field its kind, a row of OSTACLES in MAPnDAT.DTR whose first column names the mesh (track 1: cone, trestle barrier, sign). Each stands on the
 * drivable surface under it, tilted to it, heading along the nearest point of the path. When
 * one of the player's two circles comes within its radius + 200 it is thrown the way the car
 * heads at 1.4 x its speed and upwards at 0.02 x per frame (the game adds that per frame, on a
 * PC of 30 frames a second); in the air it loses 2000 units/s, falls at 500 units/s per
 * second, tumbles with its speed, bounces keeping 0.8 of its fall until that is under 50, and
 * where it would leave the drivable area it turns back with 0.3 of its speed. Then it lies on
 * its side where it stopped and can be hit again; one that stopped off the track is gone.
 *
 * Moving scenery (0x15374, 0x20944): ANIMATIONS rows of MAPnDAT.DTR are name, speed, spline;
 * the mesh drives round that spline of MAPnANM.SPL for ever at speed units/s (track 1: trucks
 * and buses over the bridges, a jet), the opponents' quadratic B-spline over the nodes'
 * handles, its height the nodes' own run straight from one to the next, facing the way it
 * goes. It moves only within 52000 units of the player, so each waits at its spline's start
 * until the player comes near.
 */
#pragma once
#include <stdint.h>
#include "track.h"

/* The props and moving scenery of track n from the DATA container dat and the GFX container
   gfx (as the data slots loaded them), their meshes into mem (cap bytes); after track_init().
   How many props and moving things there are into *props and *moving. */
void scenery_init(const uint8_t *dat, uint32_t dat_size, const uint8_t *gfx, uint32_t gfx_size, int n,
                  uint8_t *mem, uint32_t cap, int *props, int *moving);

/* One step of 1/50 s, after the player's car_step(): the player against the props, the props in
   the air, the moving scenery */
void scenery_step(void);

/* How many props the player has knocked over since scenery_init(), each counted once (the
   player's +0x7b8, for cone carnage, car.h); how many there are into *total (if given) */
int scenery_hits(int *total);

/* The props and moving scenery within the far plane's distance of cam as objects for
   track_draw_cam(), up to max */
int scenery_objects(const struct track_cam *cam, struct track_object *objs, int max);
