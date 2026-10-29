/*
 * The rasterizer as a C model: what the hardware rasterizer draws, pixel for pixel.
 * Screen positions are fixed point with 4 fraction bits (1/16 pixel), within +-2047
 * pixels. A pixel belongs to a triangle when its centre (x + 0.5, y + 0.5) lies inside or
 * on a top or left edge, so triangles that share an edge never draw a pixel twice and
 * never leave a gap. No depth buffer: whatever is drawn later covers what was drawn before
 * (painter's algorithm).
 */
#pragma once
#include <stdint.h>

struct rs_vertex { int32_t x, y; };             /* 1/16 pixel */
struct rs_clip { int x0, y0, x1, y1; };         /* drawn: x0 <= x < x1, y0 <= y < y1, within the screen */

/* One triangle in one colour into a 320-pixel-wide framebuffer; any winding, any size,
   clipped to clip. Returns the number of pixels drawn. */
uint32_t rs_triangle(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_vertex v[3], uint8_t colour);

/*
 * Textured triangles, the reference for the hardware's texture path (floating point; the
 * fixed-point version the hardware will match comes next). The same pixels as
 * rs_triangle; each pixel takes the texel at u = (u q) / q, v = (v q) / q, where q = 1/z and
 * u q, v q are interpolated linearly over the screen (perspective-correct per pixel).
 */
struct rs_tvertex { int32_t x, y; float q, uq, vq; };  /* u, v in texels */

struct rs_texture {
    const volatile uint8_t *data;               /* rows of 256 bytes */
    int width, height;                          /* u, v are clamped to them */
    int routine;                                /* 16 solid, 64 palette index 0 see-through,
                                                   224 see-through through rs_smg (below) */
};

/*
 * Routine 224 (the game's smoke, shadows and light beams, STARTL.EXE 0xcd9d0): the texel t
 * is a strength, the pixel becomes rs_smg[(t & 63) << 8 | pixel], the pixel already there
 * through row t of the track's MAPn.SMG (64 rows of 256 palette indices). Unlike the game,
 * texel 0 leaves the pixel as it is: row 0 only maps the palette's duplicate colours to
 * their first index, so it looks the same and saves the work. The hardware keeps the table
 * in the rasterizer, written through RS_SMG (hw.h, one entry a write); rs_smg is the
 * model's copy, which the firmware points at the same 16384 bytes when it writes them (0
 * reads as a table of zeros, the hardware's after configuration).
 * Its pixels are the game's for this routine (0xcd97a), not those of the others (raster.c):
 * pixel (x, r) when the point (x, r + 1) is in the triangle, its left and lower edge
 * included, so the triangles of a fan share their edges exactly and no pixel is blended
 * twice: rows a.y .. c.y - 1, in row r the columns ceil(xl) .. ceil(xr) - 1 of the left and
 * the right edge at the bottom of the row (the edges evaluated as for the others).
 */
extern const uint8_t *rs_smg;

uint32_t rs_textured(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_tvertex v[3], const struct rs_texture *tex);

/*
 * The same in fixed point, bit for bit what the hardware draws. Per triangle a reference
 * pixel and, for Q (= 1/z scaled below 2^24), U (= u/256 Q) and V, the value at its centre
 * and the change per pixel in x and y with 8 fraction bits. rs_tex_setup() makes them from
 * the floating-point vertices (returns 0 for a triangle without area).
 */
struct rs_tex_setup {
    int32_t xr, yr;                             /* reference pixel, on the screen */
    int32_t a[3];                               /* Q, U, V at its centre */
    int32_t dx[3], dy[3];                       /* per pixel, 8 fraction bits */
};

int rs_tex_setup(struct rs_tex_setup *s, const struct rs_tvertex v[3]);

/* About 2^32 / m for m in [2^15, 2^16): the hardware's reciprocal (a table of 1024 and one
   Newton step), used per pixel and by the setup unit (setup.h) */
uint32_t rs_recip(uint32_t m);
uint32_t rs_textured_fx(volatile uint8_t *fb, const struct rs_clip *clip, const struct rs_vertex pos[3],
                        const struct rs_tex_setup *s, const struct rs_texture *tex);
