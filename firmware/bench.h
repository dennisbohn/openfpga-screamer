/*
 * The benchmark ("bench = 1" in screamer.cfg, config.h): at the start the core races every track once with the gas held
 * (the simulation's race: the start lights' countdown at once, ten cars, the first car, the options as set), for
 * BENCH_FRAMES pictures each, and writes what the frames took to debug.txt, one line a track, and to the screen.
 * The frames are those of ftime.h: the work from the swap before a picture to the end of the picture.
 */
#pragma once
#include <stdint.h>
#include "ftime.h"

#define BENCH_TRACKS    6
#define BENCH_FRAMES    250         /* pictures counted a track (the first, with the countdown's physics, is not) */
#define BENCH_LOG_BYTES 1200        /* kept free in debug.txt for the result (dlog_keep) */

/* one track's line for debug.txt, with the settings in force:
   "bench track 1: far 2.00 lod 2.00 tiny 1.00, 250 frames avg 16.6 max 21.6 late 2 faces 1730 / 1961 pieces 340 / 377 tiny 12" */
void bench_log_row(int track, const struct ft_summary *s, float far_scale, float lod_scale, float tiny_px);

/* the screen's line for one track (40 columns): " 1  16.6  21.6   2  1730  340   12", a track that could not be
   set up as " 1  -" */
void bench_screen_row(char *line, int track, const struct ft_summary *s);
#define BENCH_SCREEN_HEAD   "T   avg   max late  faces pieces tiny"

/* the log's whole result: a line for each track that ran, after the header with the settings */
void bench_log(const struct ft_summary *rows, const int *ok, float far_scale, float lod_scale, float tiny_px);
