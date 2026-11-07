/*
 * The blitter (BL) commands (core/src/fpga/core/blit.v, docs/soc.md) and the C model of the
 * blitter. The blitter draws pictures, characters and lines into the framebuffer one pixel at
 * a time, and rectangles of one colour one word (four pixels) at a time. It runs once the 3D
 * units are done (the race screen), or while they wait for the CPU (the backdrop, before the
 * scene).
 * The CPU builds a list of commands with blit_start() and the blit_* functions. The hardware
 * takes the list; blit_model() draws the same pixels on the PC.
 */
#pragma once
#include <stdint.h>

#define BLIT_COPY   0               /* the bytes as they are, 0 see-through */
#define BLIT_FONT   1               /* shades 1..15 as colour * 16 + shade, 0 see-through */
#define BLIT_LINE   2               /* Bresenham, the pixels in the clip */
#define BLIT_FILL   3               /* a rectangle of one colour */
#define BLIT_OPAQUE 16              /* with BLIT_COPY: 0 drawn as well */
#define BLIT_MAX    384             /* commands in a list: the race screen with the radar's walls
                                       needs that many. The blitter's own buffer holds 128 and
                                       BL_PUSH waits while it is full. */

struct blit_list {
    int n, buffer;                  /* commands in the list; the framebuffer drawn into */
    uint32_t c[BLIT_MAX][4];        /* the commands' words (blit.c has the layout). A copy has the
                                       SDRAM address of its pixels left out; src[] has it. */
    const uint8_t *src[BLIT_MAX];   /* a copy's first pixel */
};

/* One font of the race screen's characters (GAMEL.FNT, tools/make_track.py). first and count
   give the character codes it has; the image of character ch is first_image + (ch - first). */
struct blit_font {
    uint32_t height, first, count, widest, first_image;
};
/* One picture of the race screen section: its offset from the section, its size, its place */
struct blit_image { uint32_t off; uint16_t w, h; int16_t x, y; };

/* Empties the list l. buffer is the framebuffer that the list draws into. */
void blit_start(struct blit_list *l, int buffer);

/* Adds a picture w x h whose first pixel goes to (x, y). The rows of src are stride bytes
   apart (up to 1023). kind is BLIT_COPY (with BLIT_OPAQUE if wanted) or BLIT_FONT; colour
   is used by BLIT_FONT. A command with w or h outside 1..511, or one that does not fit in
   the list, is dropped. */
void blit_copy(struct blit_list *l, const uint8_t *src, int w, int h, int stride, int x, int y, int kind,
               int colour);

/* Adds the rectangle w x h at (x, y) in one colour. */
void blit_fill(struct blit_list *l, int x, int y, int w, int h, int colour);

/* Adds a line from (x0, y0) to (x1, y1). Only its pixels inside [cx0, cx1) x [cy0, cy1) are drawn. */
void blit_line(struct blit_list *l, int x0, int y0, int x1, int y1, int cx0, int cy0, int cx1, int cy1,
               int colour);

/*
 * Sets text as the game does (original: STARTL.EXE 0x24fc0). Each character takes its width
 * and an empty column, and one more empty column before it unless that is switched off.
 * Control bytes in the string: 1 = fixed pitch (every character takes the widest character's
 * width + 2 and is centred in it), 2 = proportional, 3 = no empty column before a character,
 * 4 = the empty column is back. Characters that the font does not have are left out.
 * base and img are the race screen section and its table of images. The text starts at
 * (x, y) and is drawn only if draw is not 0. Returns its width.
 */
int blit_text(struct blit_list *l, const uint8_t *base, const struct blit_image *img,
              const struct blit_font *f, int x, int y, const char *s, int colour, int draw);

/* The C model: draws the commands of the list into fb (320 x 200 bytes). */
void blit_model(volatile uint8_t *fb, const struct blit_list *l);
