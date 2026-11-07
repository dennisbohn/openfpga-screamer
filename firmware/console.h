/*
 * Text and boxes in a framebuffer, and number formatting without printf.
 * Text uses the 8x8 font of font8x8_basic.h on a grid of 40 x 25 character cells. Each cell is
 * drawn opaque (foreground and background colour), with two word stores per pixel row.
 */
#pragma once
#include <stdint.h>

#define CON_COLS    40
#define CON_ROWS    25

/* Colours are indices into the palette that pal_write() makes (docs/soc.md). In that palette
   red + green is always 255, so it has neither black nor white: entry 180 is the darkest and
   entry 0 the brightest. They are 3.9 : 1 apart in contrast. */
#define COL_BG      180     /* rust   0xB44B1C */
#define COL_TEXT    0       /* green  0x00FF00 */
#define COL_DIM     42      /* mint   0x2AD57E */
#define COL_ALERT   230     /* pink   0xE619B2, background of failures */
#define COL_SQUARE  85      /* blue   0x55AAFF */

/* Write the palette of the back buffer: pal[i] = (i << 16) | ((255 - i) << 8) | ((i * 3) & 255) */
void pal_write(void);

/* Fill a rectangle with colour c. It is not clipped. It stores bytes up to a word boundary,
   then words. */
void fb_fill(volatile uint8_t *fb, int x, int y, int w, int h, uint8_t c);

/* Draw the string s at character cell (col, row), clipped to the grid. Returns the column
   after the text. */
int con_text(volatile uint8_t *fb, int col, int row, const char *s, uint8_t fg, uint8_t bg);

/* Formatting. Each function writes a NUL-terminated string at p and returns the address of
   the NUL, so calls chain: p = fmt_str(p, "frame "); p = fmt_udec(p, n); */
char *fmt_str(char *p, const char *s);
char *fmt_udec(char *p, uint32_t v);
char *fmt_dec(char *p, int32_t v);
char *fmt_hex(char *p, uint32_t v, int digits);     /* exactly `digits` hex digits, lower case */
/* A factor with two decimals ("1.50"), or "off" for 0 (the far and lod values of screamer.cfg) */
char *fmt_scale(char *p, float v);
