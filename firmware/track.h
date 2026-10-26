/*
 * The track flythrough: one track from the game files (tools/make_track.py, data slot 3)
 * seen from a camera that drives the player's line. The CPU does the geometry the way the
 * geometry unit will: the pieces within reach, sorted far to near (the game's painter's
 * algorithm), each piece's faces in its stored order for the side the camera is on, faces
 * turned away left out; the faces go in view space to the setup unit, which cuts,
 * projects and draws them.
 */
#pragma once
#include <stdint.h>
#include "raster.h"
#include "setup.h"

/* Whether a track file is at file (its header); then its blocks can be checked with
   track_block_ok() before track_init() reads the rest. track_init() returns the number of
   placements, 0 if the file does not fit the limits. */
int track_open(const volatile uint8_t *file);
int track_init(void);

/* The track's palette, 256 x 0x00RRGGBB */
const uint32_t *track_palette(void);

/* Path points of the camera's lap */
uint32_t track_path_length(void);

/* The game's DETAIL option, 0 LOW, 1 MID, 2 HI (the default): how deep faces are drawn.
   The track file goes with it: the L set at LOW, the H set from MID up. */
void track_detail(int level);

/* The file's size and its blocks of 256 KB from offset 0x100 on, each with a CRC-32 in the
   header: whether block k (0 .. track_blocks() - 1) arrived as written */
uint32_t track_size(void);
int track_blocks(void);
int track_block_ok(int k);

struct track_stats {
    uint32_t pieces;                            /* placements drawn */
    uint32_t faces;                             /* faces drawn */
    uint32_t triangles;                         /* triangles handed out */
    uint32_t see_through;                       /* faces of routine 224 left out for now */
    int32_t cam[3];                             /* camera X, Y, height */
    uint32_t nearest;                           /* distance to the nearest placement */
    uint32_t vertices;                          /* vertices taken into view space */
    /* CPU cycles (0 on the PC): pieces in view and sorted, vertices into view space, the
       face rows (backface test, material), handing the faces on (the hook or the C model) */
    uint32_t cyc_cull, cyc_vert, cyc_face, cyc_out;
};

/* Where faces go, in view space: the setup unit's C model and the rasterizer's into fb
   (TRACK_MODEL), or the hook (TRACK_HW; tex 0 for one colour) */
#define TRACK_MODEL     0
#define TRACK_HW        1
extern int track_mode;
extern void (*track_hw_poly)(const struct su_corner *c, int n, const struct rs_texture *tex, uint8_t colour);

/* The projection for clip (for the setup unit's registers) */
void track_view(const struct rs_clip *clip, struct su_view *v);

/* The view from path position pos (path points, 8 fraction bits), clipped to clip */
void track_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t pos, struct track_stats *st);
