/*
 * Texture demo: a small scene from one track's textures (tools/make_textures.py, data
 * slot 3 in the SDRAM), turning, drawn with rs_textured() back to front.
 */
#pragma once
#include <stdint.h>
#include "raster.h"
#include "setup.h"

/* how texdemo_draw() draws: the floating-point reference, the C models of the setup unit
   and the rasterizer, or through texdemo_hw (a quad in view space to the setup unit; fb is
   not used then and the pixel count is texdemo_hw's) */
#define TEXDEMO_FLOAT   0
#define TEXDEMO_FIXED   1
#define TEXDEMO_HW      2
extern int texdemo_mode;
extern uint32_t (*texdemo_hw)(const struct su_corner c[4], const struct rs_texture *t);

/* The projection for clip (for the setup unit's registers) */
void texdemo_view(const struct rs_clip *clip, struct su_view *v);

/* Write a stand-in texture file (test patterns, test palette) at file */
void texdemo_synth(volatile uint8_t *file);

/* Check the texture file at file; returns the number of textures, 0 if none was loaded. */
int texdemo_init(const volatile uint8_t *file);

/* The file's palette, 256 x 0x00RRGGBB */
const volatile uint32_t *texdemo_palette(void);

/* Draw the scene at angle a (1/256 turn) into fb, clipped to clip; returns pixels drawn
   and the number of triangles in *tris. */
uint32_t texdemo_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t a, uint32_t *tris);
