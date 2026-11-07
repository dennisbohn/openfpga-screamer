/*
 * The race's frame times, for the timing overlay and for the first line of debug.txt. Both belong to the
 * debug build (debug.h). The overlay is switched on and off by A, L and R together.
 *
 * The work of a frame is counted in CPU cycles, from the start of the frame (the swap before it) to the end
 * of the picture: the rasterizer is done and, if the overlay is on, the overlay is drawn. At 50 pictures a
 * second the work has to stay under 20 ms (FT_LATE cycles). A longer frame is "late".
 * The cycles come from the counter that the frame timeline already reads (rdcycle). Counting costs one read
 * at the end of a frame and four inside it, a few dozen instructions in all. The overlay costs its text only
 * while it is on.
 */
#pragma once
#include <stdint.h>

#define FT_LATE     1485000u        /* 20 ms at the CPU's 74.25 MHz */

/* What a frame was made of. work and the five times are in CPU cycles, and together they make up work. The
   letter in front of each time is the one that ft_head writes before it in debug.txt's first line. */
struct ft_frame {
    uint32_t work;
    uint32_t pre;                   /* a: before the scene: music, physics, backdrop, set-up */
    uint32_t cull;                  /* c: the scene's cull and sort */
    uint32_t pieces;                /* s: handing the pieces over */
    uint32_t post;                  /* h: after the last piece up to the wait: effects, radar, race screen */
    uint32_t wait;                  /* w: waiting for the rasterizer */
    uint32_t faces, npieces;        /* f, p: faces looked at, pieces handed on */
    uint32_t where;                 /* m: where on the track, the marker the car reached */
};

/* What a run of frames (since ft_reset) came to. This is the benchmark's result for a track. avg and max are
   in cycles, the other values are counts. Faces and pieces are given as the mean per frame and as the
   longest frame had them. */
struct ft_summary {
    uint32_t frames, avg, max, late;
    uint32_t faces_avg, faces_worst, pieces_avg, pieces_worst;
};
void ft_summary(struct ft_summary *s);
/* Writes cycles as milliseconds with one decimal ("12.3") at p, and returns a pointer to the end (the
   terminating 0). */
char *ft_ms(char *p, uint32_t cycles);

void ft_reset(void);                /* a new race: the statistics start again; the overlay stays as it is */
void ft_overlay_toggle(void);       /* switches the overlay; its maximum and late count start again */
int ft_overlay_on(void);
void ft_frame(const struct ft_frame *f);    /* adds one finished frame to the statistics and the overlay */
/* Writes the race so far as the first line of debug.txt carries it, after p. The mean, the maximum and the
   late frames are over the whole race. After the bar come the parts of the longest frame (see struct
   ft_frame; f faces, p pieces, m marker):
       " avg 11.2 max 17.9 late 0 | a1.3 c1.4 s7.1 h0.3 w0.0 f873 p219 m5"
   Returns a pointer to the end. */
char *ft_head(char *p);
/* Draws the overlay into the framebuffer fb, in colours fg on bg. It has three lines at the top left:
   the last frame in ms, the mean of the last second, the maximum and the late count since the overlay was
   switched on; the faces and pieces of the last frame; and the settings in force (screamer.cfg: far and lod).
   The numbers are those of the frames up to the last one. */
void ft_draw(volatile uint8_t *fb, uint8_t fg, uint8_t bg, float far_scale, float lod_scale);
