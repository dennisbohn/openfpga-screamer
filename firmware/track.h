/*
 * The track flythrough: one track from the game files (tools/make_track.py, data slot 3)
 * seen from a camera that drives the player's line. The CPU picks the pieces within reach
 * and sorts them far to near (the game's painter's algorithm); each piece then goes with its
 * place in view space to the geometry unit
 * (geom.h), which reads the mesh, leaves out the faces turned away and hands the others to
 * the setup unit.
 */
#pragma once
#include <stdint.h>
#include "raster.h"
#include "setup.h"
#include "geom.h"

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
    uint32_t pieces;                            /* placements handed to the geometry unit */
    /* counted by the C model (with the hardware 0 here, its registers have them): faces
       drawn, triangles, faces of routine 224 left out for now, vertices into view space */
    uint32_t faces, triangles, see_through, vertices;
    int32_t cam[3];                             /* camera X, Y, height */
    uint32_t nearest;                           /* distance to the nearest placement */
    /* CPU cycles (0 on the PC): pieces in view and sorted, each piece's place in view
       space, handing the pieces on (the hook or the C models) */
    uint32_t cyc_cull, cyc_piece, cyc_out;
};

/* Where pieces go: the C models of the geometry unit, the setup unit and the rasterizer
   into fb (TRACK_MODEL), or the hook (TRACK_HW) */
#define TRACK_MODEL     0
#define TRACK_HW        1
extern int track_mode;
extern void (*track_hw_piece)(const struct gu_piece *p);

/* The track's material file (for the geometry unit's register) */
const uint8_t *track_mlb(void);

/* The projection for clip (for the setup unit's registers) */
void track_view(const struct rs_clip *clip, struct su_view *v);

/* The view from path position pos (path points, 8 fraction bits), clipped to clip */
void track_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t pos, struct track_stats *st);
