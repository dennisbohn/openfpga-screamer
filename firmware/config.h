/*
 * screamer.cfg: an optional text file that sets how far the core draws. It is data slot 26 of core/data.json
 * and lies in Assets/screamer/common/. Each setting is a factor on the original's distances (track.h):
 *     far = 1.5      the far plane, the reach and the cell window; 1.0 is the original's picture
 *     lod = 1.5      where a block takes the next lower level of detail; 0 or "off" means none
 *     bench = 1      debug build only: the benchmark (bench.h). At the start the core races every track
 *                    once with the gas held. It writes the mean and worst frame time, the late frames, the
 *                    faces and the pieces of each track to debug.txt and to the screen.
 * Each setting is one line, "name = number". A comma works as the decimal point and a trailing x is ignored.
 * A # or ; starts a comment. Without the file, or for a line that is not understood, the constants of track.h
 * stay. The numbers are kept within 0.5 to 4.0 for far, and 0 or 0.5 to 4.0 for lod. Values above 2.0 are
 * accepted but have not been measured.
 */
#pragma once
#include <stdint.h>

#define CONFIG_SLOT     26
#define CONFIG_MAX      1024        /* a larger file is ignored (data.json: size_maximum) */

/* What config_parse found. */
#define CFG_FAR         1u          /* far was set */
#define CFG_LOD         2u          /* lod was set */
#define CFG_CLAMPED     4u          /* a number was outside its range and was moved into it */
#define CFG_BAD         8u          /* a line was not understood (unknown name, no number) and was left out */
#define CFG_BENCH       16u         /* bench was set */

/* Parses the text s of n bytes. The values it finds replace *far_scale and *lod_scale, or set *bench; the
   others stay as they are. Returns a mask of CFG_* bits. */
unsigned config_parse(const char *s, uint32_t n, float *far_scale, float *lod_scale, int *bench);

/* Reads the file, if there is one, sets the factors (track_set_scales) and writes what happened to
   debug.txt. */
void config_load(void);
/* Returns 1 if "bench = 1" was read. A release always returns 0. */
int config_bench(void);
