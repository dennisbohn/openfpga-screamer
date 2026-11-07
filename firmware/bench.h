/*
 * The benchmark ("bench = 1" in screamer.cfg, see config.h; debug build only). At the start the core races
 * every track once with the gas held. It is the simulation's race: the countdown of the start lights runs at
 * once, there are ten cars, the first car is used, and the options are as set. Each track runs for
 * BENCH_FRAMES pictures. The frame times (ftime.h: the work from the swap before a picture to the end of the
 * picture) go to debug.txt, one line a track, and to the screen.
 */
#pragma once
#include <stdint.h>
#include "ftime.h"

#define BENCH_TRACKS    6
#define BENCH_FRAMES    250         /* pictures counted for a track; the first one, which runs the countdown's
                                       physics, is not counted */
#define BENCH_LOG_BYTES 1200        /* bytes kept free in debug.txt for the result (dlog_keep) */

/* Writes one track's line to debug.txt, with the settings in force. For example:
   bench track 1: far 2.00 lod 2.00, 250 frames avg 16.6 max 21.6 late 2 faces 1730 / 1961 pieces 340 / 377
*/
void bench_log_row(int track, const struct ft_summary *s, float far_scale, float lod_scale);

/* Writes the screen's line for one track (40 columns) at line. For example:
   "1  16.6  21.6    2   1730    340"
   The columns are under the heading BENCH_SCREEN_HEAD. A track that could not be set up (s is 0 or has no
   frames) gives "1  -". */
void bench_screen_row(char *line, int track, const struct ft_summary *s);
#define BENCH_SCREEN_HEAD   "T   avg   max late  faces pieces"

/* Writes the whole result to debug.txt: a header line, then a line for each track. rows[t] is valid if ok[t]
   is set; for any other track the line says that it could not be set up. */
void bench_log(const struct ft_summary *rows, const int *ok, float far_scale, float lod_scale);
