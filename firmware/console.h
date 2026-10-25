/*
 * Text and boxes in a framebuffer, and number formatting without printf.
 * Text uses the 8x8 font of font8x8_basic.h on a grid of 40 x 25 character cells; each
 * cell is drawn opaque (fg and bg), two word stores per pixel row.
 */
#pragma once
#include <stdint.h>

#define CON_COLS    40
#define CON_ROWS    25

/* Colours: indices into the palette of docs/soc.md, see pal_write(). Red + green is always
   255 there, so there is no black or white: 180 is the darkest entry, 0 the brightest,
   3.9 : 1 apart. */
#define COL_BG      180     /* rust   0xB44B1C */
#define COL_TEXT    0       /* green  0x00FF00 */
#define COL_DIM     42      /* mint   0x2AD57E */
#define COL_ALERT   230     /* pink   0xE619B2, background of failures */
#define COL_SQUARE  85      /* blue   0x55AAFF */

/* Write the palette of the back buffer: pal[i] = (i << 16) | ((255 - i) << 8) | ((i * 3) & 255) */
void pal_write(void);

/* Fill a rectangle (not clipped) with colour c: bytes up to a word boundary, then words. */
void fb_fill(volatile uint8_t *fb, int x, int y, int w, int h, uint8_t c);

/* Draw s at character cell (col, row), clipped to the grid. Returns the column after it. */
int con_text(volatile uint8_t *fb, int col, int row, const char *s, uint8_t fg, uint8_t bg);

/* Formatting: each writes a NUL-terminated string at p and returns the NUL, so calls chain:
   p = fmt_str(p, "frame "); p = fmt_udec(p, n); */
char *fmt_str(char *p, const char *s);
char *fmt_udec(char *p, uint32_t v);
char *fmt_dec(char *p, int32_t v);
char *fmt_hex(char *p, uint32_t v, int digits);
