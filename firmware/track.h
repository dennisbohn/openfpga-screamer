/*
 * The track flythrough: one track from the game files (tools/make_track.py, data slot 3)
 * seen from a camera that drives the player's line. The CPU does the geometry the way the
 * geometry unit will: the pieces within reach, sorted far to near (the game's painter's
 * algorithm), each piece's faces in its stored order for the side the camera is on, faces
 * turned away left out, cut at the near plane and a guard band, projected, and handed out
 * as triangles.
 */
#pragma once
#include <stdint.h>
#include "raster.h"

/* Check the track file at file; returns the number of placements, 0 if none was loaded. */
int track_init(const volatile uint8_t *file);

/* The track's palette, 256 x 0x00RRGGBB */
const uint32_t *track_palette(void);

/* Path points of the camera's lap */
uint32_t track_path_length(void);

struct track_stats {
    uint32_t pieces;                            /* placements drawn */
    uint32_t faces;                             /* faces drawn */
    uint32_t triangles;                         /* triangles handed out */
    uint32_t see_through;                       /* faces of routine 224 left out for now */
};

/* Where triangles go: the C model into fb (TRACK_MODEL) or the two hooks (TRACK_HW) */
#define TRACK_MODEL     0
#define TRACK_HW        1
extern int track_mode;
extern void (*track_hw_tex)(const struct rs_vertex pos[3], const struct rs_tex_setup *s, const struct rs_texture *t);
extern void (*track_hw_flat)(const struct rs_vertex pos[3], uint8_t colour);

/* The view from path position pos (path points, 8 fraction bits), clipped to clip */
void track_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t pos, struct track_stats *st);
