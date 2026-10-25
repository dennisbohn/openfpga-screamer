/*
 * Text and boxes in a framebuffer (console.h). The font is font8x8_basic.h from
 * github.com/dhepper/font8x8 (commit 8e279d2, public domain), unchanged: 8 bytes per
 * character, one per pixel row, bit 0 = leftmost pixel.
 */
#include "hw.h"
#include "console.h"
#include "font8x8_basic.h"

/* Byte k of nibble_mask[n] is 0xFF when bit k of n is set: four pixels of a font row at
   once (little-endian, so the leftmost pixel is the lowest byte). */
static const uint32_t nibble_mask[16] = {
    0x00000000u, 0x000000FFu, 0x0000FF00u, 0x0000FFFFu, 0x00FF0000u, 0x00FF00FFu, 0x00FFFF00u, 0x00FFFFFFu,
    0xFF000000u, 0xFF0000FFu, 0xFF00FF00u, 0xFF00FFFFu, 0xFFFF0000u, 0xFFFF00FFu, 0xFFFFFF00u, 0xFFFFFFFFu,
};

void pal_write(void)
{
    for (uint32_t i = 0; i < 256; i++)
        VID_PAL[i] = (i << 16) | ((255 - i) << 8) | ((i * 3) & 255);
}

void fb_fill(volatile uint8_t *fb, int x, int y, int w, int h, uint8_t c)
{
    uint32_t c4 = c * 0x01010101u;
    for (volatile uint8_t *row = fb + y * FB_W + x; h > 0; h--, row += FB_W) {
        volatile uint8_t *p = row;
        int n = w;
        for (; n > 0 && ((uintptr_t)p & 3); n--)
            *p++ = c;
        for (; n >= 4; n -= 4, p += 4)
            *(volatile uint32_t *)p = c4;
        for (; n > 0; n--)
            *p++ = c;
    }
}

int con_text(volatile uint8_t *fb, int col, int row, const char *s, uint8_t fg, uint8_t bg)
{
    uint32_t f = fg * 0x01010101u, b = bg * 0x01010101u;
    if (row < 0 || row >= CON_ROWS)
        return col;
    for (; *s && col < CON_COLS; s++, col++) {
        if (col < 0)
            continue;
        const uint8_t *glyph = (const uint8_t *)font8x8_basic[(uint8_t)*s & 0x7F];
        volatile uint32_t *p = (volatile uint32_t *)(fb + row * 8 * FB_W + col * 8);
        for (int y = 0; y < 8; y++, p += FB_W / 4) {
            p[0] = b ^ ((f ^ b) & nibble_mask[glyph[y] & 15]);
            p[1] = b ^ ((f ^ b) & nibble_mask[glyph[y] >> 4]);
        }
    }
    return col;
}

char *fmt_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

char *fmt_udec(char *p, uint32_t v)
{
    char digits[10];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = digits[--n];
    *p = 0;
    return p;
}

char *fmt_dec(char *p, int32_t v)
{
    if (v < 0) {
        *p++ = '-';
        return fmt_udec(p, 0u - (uint32_t)v);
    }
    return fmt_udec(p, (uint32_t)v);
}

/* Lower case, like the addresses in firmware.lst */
char *fmt_hex(char *p, uint32_t v, int digits)
{
    while (digits-- > 0)
        *p++ = "0123456789abcdef"[(v >> (4 * digits)) & 15];
    *p = 0;
    return p;
}
