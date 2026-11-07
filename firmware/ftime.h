/*
 * The race's frame times, for the timing overlay (A, L and R together) and debug.txt's first line.
 *
 * A frame's work is the CPU's cycles from the start of the frame (the swap before it) to the end
 * of the picture: the rasterizer done and, if it is on, the overlay drawn. At 50 pictures a
 * second it has to stay under 20 ms (FT_LATE cycles); a longer one is "late". The cycles come
 * from the counter the frame timeline already reads (rdcycle), one read at the end of a frame
 * and four inside it, a few dozen instructions in all; the overlay costs its text only while it
 * is on.
 */
#pragma once
#include <stdint.h>

#define FT_LATE     1485000u        /* 20 ms at the CPU's 74.25 MHz */

/* what a frame was made of, in cycles (their sum is work) and counts */
struct ft_frame {
    uint32_t work;
    uint32_t pre;                   /* before the scene: music, physics, backdrop, set-up (a) */
    uint32_t cull;                  /* the scene's cull and sort (c) */
    uint32_t pieces;                /* handing the pieces over (s) */
    uint32_t post;                  /* after the last piece up to the wait: effects, radar, race screen (h) */
    uint32_t wait;                  /* waiting for the rasterizer (w) */
    uint32_t faces, npieces;        /* faces looked at, pieces handed on */
    uint32_t where;                 /* where on the track: the marker the car reached */
    uint32_t tiny;                  /* placements left out for being tiny (track_set_tiny) */
};

void ft_reset(void);                /* a new race: the statistics start again (the overlay stays as it is) */
void ft_overlay_toggle(void);       /* the overlay on or off; on, its maximum and late count start again */
int ft_overlay_on(void);
void ft_frame(const struct ft_frame *f);
/* the race so far as debug.txt's first line carries it, after p: " avg 11.2 max 17.9 late 0 | a1.3 c1.4 s7.1 h0.3 w0.0 f873 p219 m5" */
char *ft_head(char *p);
/* the overlay (three lines, top left) into the framebuffer fb, in colours fg on bg; the numbers are the frames up to the last;
   the third line gives the settings in force (screamer.cfg: far, lod, tiny) and how many placements the last frame left out */
void ft_draw(volatile uint8_t *fb, uint8_t fg, uint8_t bg, float far_scale, float lod_scale, float tiny_px);
