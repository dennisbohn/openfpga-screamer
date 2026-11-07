/*
 * The setup unit (SU): a C model of core/src/fpga/core/setup.v. It computes bit for bit what
 * the hardware computes.
 *
 * Inputs: the geometry unit (geom.h) hands over polygons of 3 or 4 corners in view space.
 * Work: the unit cuts a polygon where it leaves the space between the near plane, the far
 * plane and a guard band. It projects the corners, splits the polygon into a fan of triangles
 * (corner 0 with each pair of neighbours) and works out for each triangle what the rasterizer
 * needs.
 * Outputs: for each triangle the screen positions and, for a textured triangle, the planes of
 * Q, U and V (raster.h, rs_tex_setup). su_polygon8() then draws them with the rasterizer model.
 *
 * Number formats ("n fraction bits" means a value times 2^n):
 *   - Input corner: r, u, f, s and t in fixed point with 8 fraction bits. Each value is
 *     truncated towards zero and saturated at +-(2^31 - 1). s and t are saturated at +-2^23.
 *   - Cut (su_clip): near plane f >= 128, guard band |r|, |u| <= 14 f, far plane f <= the
 *     view's far depth. A polygon is cut only at the planes that one of its corners lies
 *     outside of, in that order. A new corner on an edge from a to b lies at
 *     t = |da| / (|da| + |db|) with 16 fraction bits (from the reciprocal table), and each of
 *     its values is a + ((b - a) t >> 16).
 *   - Corner: rc = 2^(16 + l) / f from the reciprocal table and one Newton step (raster.c,
 *     rs_recip), where l is the bit length of f. The screen position is
 *     x = centre + scale r / f and y = centre - scale u / f in 1/16 pixel. s rc and t rc are
 *     kept for the texture planes.
 *   - Triangle: d is twice its area in 1/256 pixel^2; a triangle with d = 0 is left out.
 *     Q = 1/f is scaled so that the largest Q of the triangle is at most 2^23, and U = s Q / 256
 *     and V = t Q / 256. Their change per pixel comes from the plane through the three corners
 *     with 1/d from the table. The value at the centre of the reference pixel is also computed.
 * See setup.c for every rounding.
 */
#pragma once
#include <stdint.h>
#include "raster.h"

/* The projection. cx, cy: the screen position of the view axis in 1/16 pixel. scale: the focal
   length in pixels times 16. far8: the far plane's depth with 8 fraction bits. */
struct su_view { int32_t cx, cy; uint32_t scale; int32_t far8; };

#define SU_NEAR8    (128 * 256)     /* the near plane: depth 128 (original: STARTL.EXE 0x729ac) */
#define SU_GUARD    14              /* |r|, |u| <= 14 depth: within +-1792 pixels at focal 128 */
#define SU_MAX_IN   4               /* corners of a polygon handed in */
#define SU_MAX_POLY 10              /* corners after cutting at six planes */

/* A corner with 8 fraction bits: r, u, f, and s, t within +-2^23 */
struct su_fixed { int32_t r, u, f, s, t; };

/* A corner after projection */
struct su_point {
    int32_t x, y;                   /* 1/16 pixel */
    uint32_t rc;                    /* 2^(16 + l) / f, at most 2^17 */
    int l;                          /* bit length of f with 8 fraction bits */
    int64_t sr, tr;                 /* s rc and t rc (s, t with 8 fraction bits) */
};

/* Cuts the polygon in (n corners) at the six planes into out. Returns the number of corners
   left, or 0 if fewer than 3 are left. */
int su_clip(const struct su_view *v, const struct su_fixed *in, int n, struct su_fixed *out);

/* Projects corner c into *p: the screen position cut to whole pixels, and rc, l, s rc, t rc
   for the texture planes. */
void su_project(const struct su_view *v, const struct su_fixed *c, struct su_point *p);

/* Works out the triangle p0, p1, p2: its screen positions into pos and, for a textured
   triangle, the texture setup into *s. Returns 0 for a textured triangle without area (the
   hardware leaves it out). */
int su_triangle(const struct su_point p[3], struct rs_vertex pos[3], struct rs_tex_setup *s, int textured);

/* Draws a polygon of n corners (3 or 4), given in fixed point as the geometry unit hands it
   over. The model cuts it, projects it and splits it into a fan of triangles. The rasterizer
   model draws each into fb: rs_textured_fx() with tex, or rs_triangle() in colour if tex is 0.
   Returns the pixels drawn. */
uint32_t su_polygon8(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                     const struct su_fixed *c, int n, const struct rs_texture *tex, uint8_t colour);
