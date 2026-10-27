/*
 * The blitter's commands (blit.v, docs/soc.md) and its C model: pictures, characters and
 * lines into the framebuffer, one pixel at a time, and rectangles of one colour, a word of
 * four pixels at a time; once the 3D units are done (the race screen), or while they wait
 * for the CPU (the backdrop, before the scene).
 */
#pragma once
#include <stdint.h>

#define BLIT_COPY   0               /* the bytes as they are, 0 see-through */
#define BLIT_FONT   1               /* shades 1..15 as colour * 16 + shade, 0 see-through */
#define BLIT_LINE   2               /* Bresenham, the pixels in the clip */
#define BLIT_FILL   3               /* a rectangle of one colour */
#define BLIT_OPAQUE 16              /* with BLIT_COPY: 0 drawn as well */
#define BLIT_MAX    128             /* the blitter's buffer holds 128 commands */

struct blit_list {
    int n, buffer;                  /* commands; the framebuffer drawn into */
    uint32_t c[BLIT_MAX][4];        /* the commands' words, the SDRAM address left out of copies */
    const uint8_t *src[BLIT_MAX];   /* a copy's first pixel */
};

/* the race screen's characters: one font of GAMEL.FNT (tools/make_track.py) */
struct blit_font {
    uint32_t height, first, count, widest, first_image;
};
/* one picture of the race screen section: offset from the section, size, place */
struct blit_image { uint32_t off; uint16_t w, h; int16_t x, y; };

void blit_start(struct blit_list *l, int buffer);
/* a picture w x h (rows stride bytes apart, up to 1023) with its first pixel at (x, y);
   kind BLIT_COPY (| BLIT_OPAQUE) or BLIT_FONT */
void blit_copy(struct blit_list *l, const uint8_t *src, int w, int h, int stride, int x, int y, int kind, int colour);
/* the rectangle w x h at (x, y) in one colour */
void blit_fill(struct blit_list *l, int x, int y, int w, int h, int colour);
/* a line from (x0, y0) to (x1, y1), its pixels inside [cx0, cx1) x [cy0, cy1) */
void blit_line(struct blit_list *l, int x0, int y0, int x1, int y1, int cx0, int cy0, int cx1, int cy1, int colour);
/* text as the game's 0x24fc0 sets it: a character takes its width and an empty column, and
   one more before it unless switched off; in the string, byte 1 = fixed pitch (every
   character the widest + 2, centred), 2 = proportional, 3 / 4 = the column before off / on;
   characters the font does not have are left out. Returns the width; draws if draw. The
   images are the race screen section's (base, table). */
int blit_text(struct blit_list *l, const uint8_t *base, const struct blit_image *img, const struct blit_font *f,
              int x, int y, const char *s, int colour, int draw);

/* the C model: the commands into fb (320 x 200 bytes) */
void blit_model(volatile uint8_t *fb, const struct blit_list *l);
