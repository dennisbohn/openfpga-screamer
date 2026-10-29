/*
 * The props by the road (the game's 0x15178; openscreamer docs/NOTES.md section 14): one per
 * node of the track's MAPnOST.SPL, the node's height field its kind, a row of OSTACLES in
 * MAPnDAT.DTR whose first column names the mesh (track 1: cone, trestle barrier, sign). Each
 * stands on the drivable surface under it, heading along the nearest point of the path. They
 * are not hit yet: they stand where the track puts them.
 */
#pragma once
#include <stdint.h>
#include "track.h"

/* The props of track n from the DATA container dat and the GFX container gfx (as the data
   slots loaded them), their meshes into mem (cap bytes); after track_init(). Returns how many
   stand (0 if the files lack them). */
int props_init(const uint8_t *dat, uint32_t dat_size, const uint8_t *gfx, uint32_t gfx_size, int n,
               uint8_t *mem, uint32_t cap);

/* The props within the far plane's distance of cam as objects for track_draw_cam(), up to max */
int props_objects(const struct track_cam *cam, struct track_object *objs, int max);
