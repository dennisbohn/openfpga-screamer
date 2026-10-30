/*
 * What the game draws through MAPn.SMG (its routine 224, 0xcd324: new pixel = SMG[texel * 256
 * + pixel]): the cars' shadows and the tyre smoke, the meshes SHADOW.3DX and SMOKE.3DX of the
 * track's GFX container drawn as objects (car.c, opp.c), and the radar's darkened window (its
 * lines and dots are the race screen's, hud.c). The geometry unit draws a face of routine 224
 * like one of routine 16 (texture, frame, texture coordinates) but blended: texel & 63 picks
 * the table's row, texel 0 leaves the pixel.
 *
 * At the track's start efx_init() takes MAPn.SMG, SHADOW.3DX and SMOKE.3DX out of the container
 * as the data slot loaded it, writes the table into the rasterizer (RS_SMG) with its row 63
 * (orange, which no mesh of a race uses) replaced by the radar's darkening, and lays out the
 * radar's quad: a mesh of one face whose material (routine 224, a texture of texels 63) lies
 * in the 32 KB before the MLB, as a face's material is a 16-bit offset from it.
 */
#pragma once
#include <stdint.h>
#include "geom.h"

/* MAPn.SMG, SHADOW.3DX and SMOKE.3DX of track n from the GFX container gfx (gfx_size bytes),
   the meshes and the radar's quad into mem (cap bytes: 64 KB, the radar's material at its end
   within 32 KB of mlb, the track's MLB); pal the track's palette (256 x 0x00RRGGBB). Returns
   1 if all of it is there; else what is missing stays out (efx_shadow() 0 and so on). */
int efx_init(const uint8_t *gfx, uint32_t gfx_size, int n, const uint8_t *mlb, const uint32_t *pal,
             uint8_t *mem, uint32_t cap);

/* 1 once MAPn.SMG went into the rasterizer; the table as it did (row 63 the radar's, 16384
   bytes, kept in mem for the C models), 0 before */
int efx_table(void);
const uint8_t *efx_smg(void);

/* The radar's darkening (below): per colour the darker one, 256 bytes; 0 without the table */
const uint8_t *efx_darken(void);
/* such a table for the palette pal (256 x 0x00RRGGBB) at f of its brightness (0x25870: the
   game builds one for every picture it darkens, 0.75) */
void efx_darken_table(const uint32_t *pal, uint8_t out[256], float f);

/* SHADOW.3DX and SMOKE.3DX as objects for the geometry unit, 0 if not loaded */
const uint8_t *efx_shadow(void);
const uint8_t *efx_smoke(void);

/* The radar's window (0x23a68): 72 x 72 pixels at (4, 124), darkened through the palette to
   0.75 (the table 0x25870 builds first, at 0x15cbc); the walls and dots inside it are clipped
   to the same box */
#define EFX_RADAR_X0    4
#define EFX_RADAR_Y0    124
#define EFX_RADAR_X1    76      /* exclusive */
#define EFX_RADAR_Y1    196

/* The radar's quad as a piece for the geometry unit, drawn after the scene with the view v
   (track_view() of the whole screen); 0 if there is none */
int efx_radar_piece(const struct su_view *v, struct gu_piece *g);

/* This frame's CPU cycles of the effects, for the log: the shadows and the smoke (their steps
   and blocks), the radar (its walls and dots for the blitter); and the radar's lines */
extern uint32_t efx_cyc_objects, efx_cyc_radar, efx_radar_lines;
