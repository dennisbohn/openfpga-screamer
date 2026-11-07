/*
 * The scenery that is not part of the track (openscreamer docs/NOTES.md section 14). There are
 * two kinds, props and moving scenery.
 *
 * Props (original: STARTL.EXE 0x15178 places them, 0x124c0 hits one, 0x205b4 moves it):
 * there is one prop for each node of the track's MAPnOST.SPL. For cone carnage the file is
 * MAPnCNG.SPL and for slalom MAPnSLA.SPL (original: 0x14fb4). The node's height field is its
 * kind, a row of OSTACLES in MAPnDAT.DTR. The first column of that row names the mesh (track 1:
 * cone, trestle barrier, sign). Each prop stands on the drivable surface under it, tilted to
 * it, and heads along the nearest point of the path.
 * When one of the player's two circles comes within the prop's radius + 200, the prop is
 * thrown. It flies the way the car heads at 1.4 x the car's speed, and upwards at 0.02 x the
 * speed per frame (the game adds that per frame, on a PC of 30 frames a second). In the air it
 * loses 2000 units/s, falls at 500 units/s per second and tumbles with its speed. It bounces,
 * keeping 0.8 of its fall speed, until that is under 50. Where it would leave the drivable
 * area it turns back with 0.3 of its speed. Then it lies on its side where it stopped and can
 * be hit once more. A prop that stopped off the track is gone.
 *
 * Moving scenery (original: STARTL.EXE 0x15374, 0x20944): a row of ANIMATIONS in MAPnDAT.DTR
 * holds a name, a speed and a spline. The mesh drives round that spline of MAPnANM.SPL for
 * ever at the speed in units/s (track 1: trucks and buses over the bridges, and a jet). The
 * path is the opponents' quadratic B-spline over the nodes' handles. The height is the nodes'
 * own run, straight from one node to the next. The mesh faces the way it goes.
 * It moves only within 52000 units of the player (the core moves this out in proportion to the
 * far plane), so each one waits at its spline's start until the player comes near.
 */
#pragma once
#include <stdint.h>
#include "track.h"

/* Sets up the props and the moving scenery of track n, after track_init(). dat and gfx are the
   DATA and GFX containers as the data slots loaded them. Their meshes go into mem (cap bytes).
   The number of props and of moving things go to *props and *moving. */
void scenery_init(const uint8_t *dat, uint32_t dat_size, const uint8_t *gfx, uint32_t gfx_size, int n,
                  uint8_t *mem, uint32_t cap, int *props, int *moving);

/* One step of 1/50 s, after the player's car_step(): the player against the props, the props in
   the air, the moving scenery. */
void scenery_step(void);

/* Returns how many props the player has knocked over since scenery_init(), each counted once
   (the player's +0x7b8, for cone carnage, car.h). The number of props goes to *total, if
   given. */
int scenery_hits(int *total);

/* Fills objs (up to max) with the props and moving scenery within the far plane's distance of
   cam, as objects for track_draw_cam(). Returns how many. */
int scenery_objects(const struct track_cam *cam, struct track_object *objs, int max);
