/*
 * screamer.cfg: how far the core draws (data slot 26 of core/data.json, an optional file in
 * Assets/screamer/common/). Two numbers, each a factor on the original's distances (track.h):
 *     far = 1.5      the far plane, the reach, the cell window; 1.0 is the original's picture
 *     lod = 1.5      where a block takes the next lower level of detail; 0 (or "off"): none
 *     tiny = 1       placements projected smaller than this many pixels across are not drawn; 0 (or "off"): all are
 * One line each, "name = number" (a comma works as the decimal point, a trailing x is ignored), # or ; starts a
 * comment. Without the file, or for a line that is not understood, the constants of track.h stay. The numbers are
 * kept within far 0.5 .. 4.0, lod 0, 0.5 .. 4.0 and tiny 0 .. 8; what is above 2.0 has not been measured.
 */
#pragma once
#include <stdint.h>

#define CONFIG_SLOT     26
#define CONFIG_MAX      1024        /* the file is ignored above this (data.json: size_maximum) */

/* what config_parse found */
#define CFG_FAR         1u          /* far was set */
#define CFG_LOD         2u          /* lod was set */
#define CFG_CLAMPED     4u          /* a number was outside its range and kept to it */
#define CFG_BAD         8u          /* a line was not understood (an unknown name, no number) and left out */
#define CFG_TINY        16u         /* tiny was set */

/* The text s of n bytes: the values it gives replace *far_scale, *lod_scale and *tiny_px, the others stay. Returns CFG_* */
unsigned config_parse(const char *s, uint32_t n, float *far_scale, float *lod_scale, float *tiny_px);

/* Read the file, if there is one, set the factors (track_set_scales) and write what happened to debug.txt */
void config_load(void);
