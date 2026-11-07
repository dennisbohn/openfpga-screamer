/*
 * The effects that the game draws through MAPn.SMG: the cars' shadows, the tyre smoke and the
 * radar's darkened window. In the original this is routine 224 (original: STARTL.EXE 0xcd324):
 * new pixel = SMG[texel * 256 + pixel].
 *   - The shadows and the smoke are the meshes SHADOW.3DX and SMOKE.3DX of the track's GFX
 *     container, drawn as objects (car.c, opp.c).
 *   - The radar's window is a quad drawn in the same way. Its lines and dots are the race
 *     screen's (hud.c).
 * The geometry unit draws a face of routine 224 like one of routine 16 (texture, frame, texture
 * coordinates), but blended: texel & 63 picks the table's row, and texel 0 leaves the pixel.
 *
 * At the track's start efx_init() takes MAPn.SMG, SHADOW.3DX and SMOKE.3DX out of the container
 * as the data slot loaded it. It writes the table into the rasterizer (RS_SMG) with its row 63
 * (orange, which no mesh of a race uses) replaced by the radar's darkening. It also lays out the
 * radar's quad, a mesh of one face. The quad's material (routine 224, a texture of texels 63)
 * must lie within reach of a face's 16-bit material offset from the MLB.
 */
#pragma once
#include <stdint.h>
#include "geom.h"

/* Loads MAPn.SMG, SHADOW.3DX and SMOKE.3DX of track n from the GFX container gfx (gfx_size
   bytes). The meshes and the radar's quad go into mem (cap bytes, 64 KB). The radar's material
   lies at the end of mem and must be within 32 KB of mlb, the track's MLB. pal is the track's
   palette (256 x 0x00RRGGBB). Returns 1 if all of it is there. Otherwise what is missing stays
   out (efx_shadow() returns 0 and so on). */
int efx_init(const uint8_t *gfx, uint32_t gfx_size, int n, const uint8_t *mlb, const uint32_t *pal,
             uint8_t *mem, uint32_t cap);

/* Returns 1 once MAPn.SMG has been written into the rasterizer, else 0. The table (16384
   bytes, with row 63 the radar's) stays in mem for the C models. */
int efx_table(void);

/* The radar's darkening table (see efx_darken_table): 256 bytes, one darker colour for each
   colour. Returns 0 if there is no table. */
const uint8_t *efx_darken(void);
/* Builds such a table for the palette pal (256 x 0x00RRGGBB) at the fraction f of the colours'
   brightness into out (original: STARTL.EXE 0x25870; the game builds one for every picture it
   darkens, the radar's with 0.75). */
void efx_darken_table(const uint32_t *pal, uint8_t out[256], float f);

/* SHADOW.3DX and SMOKE.3DX as objects for the geometry unit. They return 0 if not loaded. */
const uint8_t *efx_shadow(void);
const uint8_t *efx_smoke(void);

/* The radar's window (original: STARTL.EXE 0x23a68): 72 x 72 pixels at (4, 124), darkened
   through the palette to 0.75 (the table that 0x25870 builds first, at 0x15cbc). The walls and
   dots inside it are clipped to the same box. */
#define EFX_RADAR_X0    4
#define EFX_RADAR_Y0    124
#define EFX_RADAR_X1    76      /* exclusive */
#define EFX_RADAR_Y1    196

/* Makes the radar's quad a piece for the geometry unit into *g. It is drawn after the scene
   with the view v (track_view() of the whole screen). Returns 0 if there is no radar quad. */
int efx_radar_piece(const struct su_view *v, struct gu_piece *g);

/* This frame's figures for the debug log. efx_cyc_objects: the CPU cycles of the shadows and
   the smoke (their steps and blocks). efx_cyc_radar: the CPU cycles of the radar (its walls and
   dots for the blitter). efx_radar_lines: the number of lines the radar drew. */
extern uint32_t efx_cyc_objects, efx_cyc_radar, efx_radar_lines;
