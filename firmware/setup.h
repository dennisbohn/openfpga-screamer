/*
 * The setup unit (core/src/fpga/core/setup.v) as a C model: bit for bit what the hardware
 * computes. The CPU hands it polygons in view space; it cuts them where they leave the
 * space between the near plane, the far plane and a guard band, projects the corners,
 * splits the polygon into a fan of triangles (corner 0 with each pair of neighbours) and
 * works out per triangle what the rasterizer needs: screen positions and, textured, the
 * planes of Q, U, V (raster.h, rs_tex_setup).
 *
 * Fixed point throughout, the inputs are floats as the CPU has them:
 *   - each value to fixed point with 8 fraction bits (su_fix8: truncated towards zero,
 *     saturated at +-(2^31 - 1)); s and t saturated at +-2^23
 *   - cut (su_clip): near f >= 128, guard |r|, |u| <= 14 f, far f <= the view's far depth;
 *     only at the planes a corner of the polygon lies outside of, in that order; a new
 *     corner on an edge at t = |da| / (|da| + |db|) with 16 fraction bits (the reciprocal
 *     table), each value a + ((b - a) t >> 16)
 *   - corner: rc = 2^(16 + l) / f from the reciprocal table and one Newton step (raster.c,
 *     rs_recip), l = bit length of f; x = centre + scale r / f, y = centre - scale u / f in
 *     1/16 pixel; s rc and t rc kept for the texture planes
 *   - triangle: d = twice its area in 1/256 pixel^2 (none if 0); per triangle Q = 1/f
 *     scaled to at most 2^23 by the largest, U = s Q / 256, V = t Q / 256; their change per
 *     pixel from the plane through the three corners with 1/d from the table, and the value
 *     at the reference pixel's centre
 * See setup.c for every rounding.
 */
#pragma once
#include <stdint.h>
#include "raster.h"

/* a corner in view space: r right, u up, f forward (f > 0, after the near plane), and its
   texel position s, t */
struct su_corner { float r, u, f, s, t; };

/* the projection: the screen position of the view axis in 1/16 pixel, the focal length in
   pixels times 16, and the far plane's depth with 8 fraction bits */
struct su_view { int32_t cx, cy; uint32_t scale; int32_t far8; };

#define SU_NEAR8    (128 * 256)     /* the near plane: depth 128 (the original's, 0x729ac) */
#define SU_GUARD    14              /* |r|, |u| <= 14 depth: within +-1792 pixels at focal 128 */
#define SU_MAX_IN   4               /* corners of a polygon handed in */
#define SU_MAX_POLY 10              /* after cutting at six planes */

/* a corner with 8 fraction bits: r, u, f, and s, t within +-2^23 */
struct su_fixed { int32_t r, u, f, s, t; };

/* a corner after projection */
struct su_point {
    int32_t x, y;                   /* 1/16 pixel */
    uint32_t rc;                    /* 2^(16 + l) / f, at most 2^17 */
    int l;                          /* bit length of f with 8 fraction bits */
    int64_t sr, tr;                 /* s rc and t rc (s, t with 8 fraction bits) */
};

int32_t su_fix8(float v);
void su_fix(const struct su_corner *c, struct su_fixed *x);

/* The polygon in (n corners) cut at the planes into out; returns its corners, 0 if less
   than 3 are left */
int su_clip(const struct su_view *v, const struct su_fixed *in, int n, struct su_fixed *out);

void su_project(const struct su_view *v, const struct su_fixed *c, struct su_point *p);

/* The triangle p0, p1, p2: its screen positions, and for a textured one the texture setup.
   Returns 0 for a textured triangle without area (the hardware leaves it out). */
int su_triangle(const struct su_point p[3], struct rs_vertex pos[3], struct rs_tex_setup *s, int textured);

/* A polygon of n corners (3, 4) through the model: cut, projected, a fan of triangles,
   drawn with the rasterizer's C model into fb (rs_textured_fx with tex, else rs_triangle in
   colour). Returns the pixels drawn. */
uint32_t su_polygon(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                    const struct su_corner *c, int n, const struct rs_texture *tex, uint8_t colour);

/* The same with the corners already in fixed point (as the geometry unit hands them over) */
uint32_t su_polygon8(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                     const struct su_fixed *c, int n, const struct rs_texture *tex, uint8_t colour);
