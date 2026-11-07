/*
 * The rasterizer (RS): a C model of core/src/fpga/core/raster.v. It draws pixel for pixel what
 * the hardware rasterizer draws.
 *
 * Inputs: triangles with screen positions in 1/16 pixel (4 fraction bits), within +-2047
 * pixels, and either one colour or a texture with its setup values (struct rs_tex_setup).
 * Outputs: the pixels in a framebuffer of bytes that is 320 pixels wide, limited to a clip
 * rectangle. There is no depth buffer: what is drawn later covers what was drawn before
 * (painter's algorithm).
 *
 * Which pixels a triangle covers follows the game's fill rule (original: STARTL.EXE 0xd8844,
 * docs/original_filler.md 5): pixel (x, r) is drawn when the triangle touches the segment from
 * (x, r) to (x, r + 1), its lower end excluded. Triangles that share an edge overlap along it
 * and leave no gap. raster.c describes the rule in detail. Routine 224 (see-through) has a
 * rule of its own, described below.
 */
#pragma once
#include <stdint.h>

struct rs_vertex { int32_t x, y; };             /* 1/16 pixel */
struct rs_clip { int x0, y0, x1, y1; };         /* drawn: x0 <= x < x1, y0 <= y < y1, within the screen */

/* Draws one triangle in one colour into a framebuffer that is 320 pixels wide. Any winding and
   any size; clipped to clip. Returns the number of pixels drawn. */
uint32_t rs_triangle(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_vertex v[3], uint8_t colour);

/*
 * Textured triangles. Each pixel takes the texel at u = (u q) / q, v = (v q) / q, where
 * q = 1/z and u q, v q are interpolated linearly over the screen (perspective-correct for each
 * pixel). They cover the same pixels as rs_triangle(). rs_tvertex is the floating-point form
 * of a corner, which rs_tex_setup() (below) turns into the fixed-point values that the
 * hardware uses and that rs_textured_fx() draws with.
 */
struct rs_tvertex { int32_t x, y; float q, uq, vq; };  /* u, v in texels */

struct rs_texture {
    const volatile uint8_t *data;               /* rows of 256 bytes */
    int width, height;                          /* u, v are clamped to them */
    int routine;                                /* 16 solid, 64 palette index 0 see-through,
                                                   224 see-through through rs_smg (below), 160
                                                   solid with u, v wrapping at 256 instead (the
                                                   game's reflection, original: STARTL.EXE
                                                   0xdb59b) */
};

/*
 * Routine 224 (the game's smoke, shadows and light beams, original: STARTL.EXE 0xcd9d0): the
 * texel t is a strength. The pixel becomes rs_smg[(t & 63) << 8 | pixel]. That is the pixel
 * already there, taken through row t of the track's MAPn.SMG (64 rows of 256 palette indices).
 * Unlike the game, texel 0 leaves the pixel as it is. Row 0 only maps the palette's duplicate
 * colours to their first index, so the picture looks the same and the work is saved.
 * The hardware keeps the table in the rasterizer and the firmware writes it through RS_SMG
 * (hw.h, one entry a write). rs_smg is the model's copy: the firmware points it at the same
 * 16384 bytes when it writes them. 0 reads as a table of zeros, which is the hardware's state
 * after configuration.
 * The pixels of this routine are the game's own for it (original: STARTL.EXE 0xcd97a) and not
 * those of the other routines (raster.c). Pixel (x, r) is drawn when the point (x, r + 1) is
 * in the triangle, its left and lower edge included. So the triangles of a fan share their
 * edges exactly and no pixel is blended twice. The rows are a.y .. c.y - 1. In row r the
 * columns are ceil(xl) .. ceil(xr) - 1, where xl and xr are the left and the right edge at
 * the bottom of the row. The edges are evaluated as for the other routines.
 */
extern const uint8_t *rs_smg;

/*
 * The fixed-point setup of a textured triangle, bit for bit what the hardware draws. For each
 * triangle it holds a reference pixel and, for Q (= 1/z scaled to below 2^24), U (= u/256 Q)
 * and V, the value at the pixel's centre and the change per pixel in x and y, with 8 fraction
 * bits. rs_tex_setup() makes them from the floating-point vertices. It returns 0 for a
 * triangle without area.
 */
struct rs_tex_setup {
    int32_t xr, yr;                             /* reference pixel, on the screen */
    int32_t a[3];                               /* Q, U, V at its centre */
    int32_t dx[3], dy[3];                       /* per pixel, 8 fraction bits */
};

int rs_tex_setup(struct rs_tex_setup *s, const struct rs_tvertex v[3]);

/* About 2^32 / m for m in [2^15, 2^16): the hardware's reciprocal (a table of 1024 entries and
   one Newton step). It is used for each pixel and by the setup unit (setup.h). */
uint32_t rs_recip(uint32_t m);

/* Draws a textured triangle from its fixed-point setup (see struct rs_tex_setup). Returns the
   pixels drawn. */
uint32_t rs_textured_fx(volatile uint8_t *fb, const struct rs_clip *clip, const struct rs_vertex pos[3],
                        const struct rs_tex_setup *s, const struct rs_texture *tex);
