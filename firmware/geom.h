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
 *   - routine 160 (a car's windows, the game's 0xdb310): the texel is the reflection's u, v
 *     of the corner's vertex (no material size), and it wraps at 256 (raster.h). A block with
 *     flag 0x2000 (blk +0x18) has a normal per vertex (blk +0x20: count, then float x, y, z);
 *     for each vertex i < count the unit works out, as the original does on the x87
 *     (0xc2230), the view's reflection r = 2 (e.n) n - e (e the direction from the camera,
 *     EYE_BACK in front of the eye, to the vertex, n its normal in view space) and keeps
 *     u = round(127 r.x) + 127 - offx, v = round(127 r.z) + 127 - offz, x and z the
 *     original's view axes (left, back: -r, -f here). In integers, Q = 2^-bits:
 *       q    each normal component from its float bits, rounded to Q14
 *       n    (m q) >> 14, the normal in view space, Q16
 *       p    the vertex less EYE_BACK in f (8 fraction bits) shifted left or right until the
 *            largest |component| (one's complement) has 16 bits: 17 bits signed
 *       S    (p.n) >> 16; Q = p.p; B = ((p << 16) - 2 S n) >> 16 for r and f (B = -A)
 *       y    (127/128) 2^32 / sqrt(Q') by a table of 128 (Q' = Q, or 4 Q below 2^32: h = 1)
 *            and one Newton step: t1 = (x y0) >> 16, t2 = (t1 y0) >> 15,
 *            t3 = (3 2^16 - t2) >> 1, y = (y0 (t3 - (t3 >> 7))) >> 16 (x = Q' >> 17)
 *       u    ((B_r y << h) + 2^24) >> 25 + 127 - offx; v the same with B_f and offz
 *     Against the float form (GU_REFL_FLOAT, as the original computes it) about 1 % of the
 *     values differ by one.
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
    uint32_t refl;                      /* routine 160: [15:0] the reflection picture's phase
                                           (frame n x phase >> 16; the object's record +0x3c,
                                           the game's 0x84288), [23:16] and [31:24] the
                                           reflection's offsets of u and v (0xc1fc0, track.c) */
    const uint8_t *obj, *blk;           /* the mesh: object and its block */
};

struct gu_stats { uint32_t faces, triangles, see_through, vertices; };

/* One piece through the models of the geometry unit, the setup unit and the rasterizer
   into fb; mlb is the track's material file. Adds to *st, returns the pixels drawn. */
uint32_t gu_draw(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                 const uint8_t *mlb, const struct gu_piece *p, struct gu_stats *st);
