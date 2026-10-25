/*
 * Texture demo: a small scene from one track's textures (tools/make_textures.py, data
 * slot 3 in the SDRAM), turning, drawn with rs_textured() back to front.
 */
#pragma once
#include <stdint.h>
#include "raster.h"

/* Check the texture file at file; returns the number of textures, 0 if none was loaded. */
int texdemo_init(const volatile uint8_t *file);

/* The file's palette, 256 x 0x00RRGGBB */
const volatile uint32_t *texdemo_palette(void);

/* Draw the scene at angle a (1/256 turn) into fb, clipped to clip; returns pixels drawn
   and the number of triangles in *tris. */
uint32_t texdemo_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t a, uint32_t *tris);
