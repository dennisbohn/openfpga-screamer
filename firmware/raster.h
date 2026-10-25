/*
 * The rasterizer as a C model: what the hardware rasterizer will draw, pixel for pixel.
 * Screen positions are fixed point with 4 fraction bits (1/16 pixel). A pixel belongs to a
 * triangle when its centre (x + 0.5, y + 0.5) lies inside or on a top or left edge, so
 * triangles that share an edge never draw a pixel twice and never leave a gap. No depth
 * buffer: whatever is drawn later covers what was drawn before (painter's algorithm).
 */
#pragma once
#include <stdint.h>

struct rs_vertex { int32_t x, y; };             /* 1/16 pixel */
struct rs_clip { int x0, y0, x1, y1; };         /* drawn: x0 <= x < x1, y0 <= y < y1 */

/* One triangle in one colour into a 320-pixel-wide framebuffer; any winding, any size,
   clipped to clip. Returns the number of pixels drawn. */
uint32_t rs_triangle(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_vertex v[3], uint8_t colour);
