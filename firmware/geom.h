/*
 * The geometry unit (core/src/fpga/core/geom.v) as a C model: bit for bit what the hardware
 * does with one piece of the track (a placed mesh). The CPU picks the pieces, sorts them far
 * to near and hands each over with its place in view space; the unit reads the mesh itself
 * (UNR block, vertices, face row, faces, texture coordinates, MLB materials, all as in the
 * game files, track.c has the layout), takes the vertices into view space, leaves out the
 * faces turned away and hands the others to the setup unit (setup.h), in fixed point:
 *   - vertex: r = t_r + ((x m_rx + y m_ry + z m_rz) >> 8), u and f the same (x, y, z the
 *     mesh's int16, m with 16 fraction bits, t and the result with 8)
 *   - a face is seen when ((c0 << 8) - e) . n >= 0 (c0 its first corner, e the eye in the
 *     piece's frame with 8 fraction bits, n the face's int16 normal)
 *   - texel position with 8 fraction bits: raw u times the material's width, raw v times
 *     its height (texel = raw * size / 256)
 */
#pragma once
#include <stdint.h>
#include "raster.h"
#include "setup.h"

#define GU_MAX_VERT     512             /* vertices of a mesh; more: the piece is left out */
#define GU_MAX_FACES    1024            /* faces of a row; the rest is left out */

struct gu_piece {
    int32_t m[9];                       /* local to view: rows r, u, f of columns x, y, z; 16 fraction bits */
    int32_t t[3];                       /* the piece's origin in view space, 8 fraction bits */
    int32_t e[3];                       /* the eye in the piece's frame, 8 fraction bits */
    int row;                            /* the face row for the side the eye is on, 0..15 (track.c) */
    int mirror;                         /* a moving part's block: its frames turn the model's z up
                                           into y up, a mirror, so the faces turned away are the
                                           others (the game's 0x8acf0: jge for jl) */
    uint32_t phase;                     /* texture frame of n: n x phase >> 14 (14 bits) */
    const uint8_t *obj, *blk;           /* the mesh: object and its block */
};

struct gu_stats { uint32_t faces, triangles, see_through, vertices; };

/* One piece through the models of the geometry unit, the setup unit and the rasterizer
   into fb; mlb is the track's material file. Adds to *st, returns the pixels drawn. */
uint32_t gu_draw(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                 const uint8_t *mlb, const struct gu_piece *p, struct gu_stats *st);
